#include <gtest/gtest.h>
#include <risk/RiskManager.hpp>
#include <execution/MockExecutionEngine.hpp>
#include <core/EventBus.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace bot;
using namespace bot::test;

// ---------------------------------------------------------------------------
// RiskManager
// ---------------------------------------------------------------------------

class RiskManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<SyncEventBus> bus = std::make_shared<SyncEventBus>();
    std::shared_ptr<FakeExecutionEngine> engine = std::make_shared<FakeExecutionEngine>();
    std::shared_ptr<PortfolioManager> pm = std::make_shared<PortfolioManager>(bus, engine);

    static std::vector<LegTarget> legs(double qa, Side sa, double qb, Side sb) {
        return {LegTarget{qa, sa}, LegTarget{qb, sb}};
    }
};

TEST_F(RiskManagerTest, ApprovesWithinLimits) {
    RiskManager rm(pm, RiskLimits{10000, 0, 0, "", ""});
    EXPECT_FALSE(rm.approveEntry("b1", {"A", "B"}, legs(40, Side::BUY, 40, Side::SELL), {50, 50}));
}

TEST_F(RiskManagerTest, ReservationsCountUntilReleased) {
    // Two baskets submit at the same open; MOC fills only at the close, so the PM doesn't
    // see the first one yet. The reservation must.
    RiskManager rm(pm, RiskLimits{6000, 0, 0, "", ""});
    EXPECT_FALSE(rm.approveEntry("b1", {"A", "B"}, legs(30, Side::BUY, 30, Side::SELL), {50, 50}));  // 3000
    EXPECT_TRUE(rm.approveEntry("b2", {"C", "D"}, legs(40, Side::BUY, 40, Side::SELL), {50, 50}));   // +4000
    rm.release("b1");
    EXPECT_FALSE(rm.approveEntry("b2", {"C", "D"}, legs(40, Side::BUY, 40, Side::SELL), {50, 50}));
}

TEST_F(RiskManagerTest, PerSymbolNetCapAcrossBaskets) {
    RiskManager rm(pm, RiskLimits{0, 2500, 0, "", ""});
    EXPECT_FALSE(rm.approveEntry("b1", {"HBAN", "X"}, legs(100, Side::BUY, 1, Side::SELL), {15, 10}));  // HBAN +1500
    EXPECT_TRUE(rm.approveEntry("b2", {"HBAN", "Y"}, legs(100, Side::BUY, 1, Side::SELL), {15, 10}));   // +1500 -> 3000
    EXPECT_FALSE(rm.approveEntry("b3", {"HBAN", "Z"}, legs(100, Side::SELL, 1, Side::BUY), {15, 10})) << "offsetting is fine";
}

TEST_F(RiskManagerTest, SymbolNetUsdCountsHeldAndReservedExposure) {
    RiskManager rm(pm, RiskLimits{0, 0, 0, "", ""});
    OrderRequest r{};
    r.client_order_id = "b1_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    bus->publishExecution(FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0));
    EXPECT_EQ(rm.symbolNetUsd(), (std::map<std::string, double>{{"A", 500.0}}));

    ASSERT_FALSE(rm.approveEntry("b2", {"A", "B"}, legs(5, Side::BUY, 2, Side::SELL), {60, 20}));
    auto usd = rm.symbolNetUsd();
    EXPECT_DOUBLE_EQ(usd.at("A"), 15 * 50.0) << "held + reserved, at the held price";
    EXPECT_DOUBLE_EQ(usd.at("B"), 2 * 20.0) << "reserved only: the approval price";

    rm.release("b2");
    EXPECT_EQ(rm.symbolNetUsd(), (std::map<std::string, double>{{"A", 500.0}}));
}

TEST_F(RiskManagerTest, KillAndFlattenFiles) {
    auto dir = std::filesystem::temp_directory_path() / ("funtrade_risk_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    RiskManager rm(pm, RiskLimits{0, 0, 0, (dir / "KILL").string(), (dir / "FLATTEN").string()});
    EXPECT_FALSE(rm.killSwitchActive());
    EXPECT_FALSE(rm.flattenRequested());
    std::ofstream(dir / "KILL") << "";
    std::ofstream(dir / "FLATTEN") << "";
    EXPECT_TRUE(rm.killSwitchActive());
    EXPECT_TRUE(rm.flattenRequested());
    EXPECT_TRUE(rm.approveEntry("b1", {"A"}, {LegTarget{1, Side::BUY}}, {1}));
    std::filesystem::remove_all(dir);
}

TEST_F(RiskManagerTest, BasketLossLimit) {
    RiskManager rm(pm, RiskLimits{0, 0, 100, "", ""});
    OrderRequest r{};
    r.client_order_id = "b1_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    bus->publishExecution(FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0));
    bus->publishMarketEvent(TradeTick{"A", 45.0, 1, 1, Side::SELL, {}});
    EXPECT_FALSE(rm.basketLossBreached({"b1_A"}));  // -50
    bus->publishMarketEvent(TradeTick{"A", 39.0, 1, 2, Side::SELL, {}});
    EXPECT_TRUE(rm.basketLossBreached({"b1_A"}));   // -110
}

// ---------------------------------------------------------------------------
// MockExecutionEngine (backtest exchange)
// ---------------------------------------------------------------------------

class MockEngineTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> bus = std::make_shared<EventBus>(nullptr, nullptr, DispatchMode::Synchronous);
    std::shared_ptr<MockExecutionEngine> mock = std::make_shared<MockExecutionEngine>(bus);
    std::vector<ExecutionReport> reports;

    void SetUp() override {
        bus->subscribeToExecution([this](const ExecutionReport& r) { reports.push_back(r); });
    }
    static uint64_t ms(int64_t d) { return static_cast<uint64_t>(d) * 86'400'000ULL; }
    static OrderRequest moc(const std::string& sym, Side side, double qty) {
        OrderRequest r;
        r.client_order_id = sym + "#1"; r.symbol = sym; r.side = side; r.quantity = qty;
        r.type = OrderType::MARKET_ON_CLOSE; r.tif = TimeInForce::DAY;
        return r;
    }
};

TEST_F(MockEngineTest, MocFillsAtTheCloseOfTheSessionItWasSentIn) {
    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 10.0, 1, ms(100)});      // yesterday's close
    bus->publishMarketEvent(MidPointTick{"A", 10.5, ms(101) + 34'200'000});  // today, intraday
    EXPECT_EQ(mock->placeOrder(moc("A", Side::BUY, 5)), "A#1");
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, OrderStatus::NEW);

    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 11.0, 1, ms(101)});      // today's close
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[1].status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(reports[1].last_price, 11.0) << "official close, not the intraday price";
    EXPECT_DOUBLE_EQ(reports[1].last_quantity, 5);
    EXPECT_FALSE(reports[1].exec_id.empty());
    ASSERT_EQ(mock->getOpenPositions().size(), 1u);
    EXPECT_DOUBLE_EQ(mock->getOpenPositions()[0].quantity, 5);
}

TEST_F(MockEngineTest, MocSentOnACloseBarFillsOnThatSameBar) {
    // Bar-only data: the strategy acts on the first bar of day 101, before the exchange
    // (a wildcard listener) processes it, so the order fills at day 101's close.
    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 10.0, 1, ms(100)});
    uint64_t sub = bus->subscribeToMarketEvents("A", [this](const MarketEvent& e) {
        if (std::get<BarUpdate>(e).timestamp_ms == ms(101)) (void)mock->placeOrder(moc("A", Side::SELL, 3));
    });
    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 12.0, 1, ms(101)});
    bus->unsubscribeFromMarketEvents("A", sub);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_DOUBLE_EQ(reports[1].last_price, 12.0);
}

TEST_F(MockEngineTest, CancelPendingMoc) {
    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 10.0, 1, ms(100)});
    (void)mock->placeOrder(moc("A", Side::BUY, 5));
    mock->cancelOrder("A#1");
    bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, 11.0, 1, ms(101)});
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[1].status, OrderStatus::CANCELED);
    EXPECT_TRUE(mock->getOpenPositions().empty());
}

TEST_F(MockEngineTest, MarketOrderWithoutPriceIsRejected) {
    OrderRequest r = moc("Z", Side::BUY, 1);
    r.type = OrderType::MARKET;
    (void)mock->placeOrder(r);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, OrderStatus::REJECTED);
}

// ---------------------------------------------------------------------------
// EventBus post / flush / synchronous mode
// ---------------------------------------------------------------------------

TEST(EventBusTasks, PostRunsOnDispatcherInOrderWithEvents) {
    EventBus bus(nullptr, nullptr);
    std::vector<std::string> seen;
    std::thread::id dispatcher;
    bus.subscribeToMarketEvents("A", [&](const MarketEvent&) { seen.push_back("event"); });
    bus.publishMarketEvent(TradeTick{"A", 1, 1, 1, Side::BUY, {}});
    bus.post([&] { seen.push_back("task"); dispatcher = std::this_thread::get_id(); });
    bus.flush();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "event");
    EXPECT_EQ(seen[1], "task");
    EXPECT_NE(dispatcher, std::this_thread::get_id());
}

TEST(EventBusTasks, SynchronousModeDeliversInlineIncludingNestedPublishes) {
    EventBus bus(nullptr, nullptr, DispatchMode::Synchronous);
    std::vector<std::string> seen;
    bus.subscribeToExecution([&](const ExecutionReport& r) { seen.push_back("exec " + r.client_order_id); });
    bus.subscribeToMarketEvents("A", [&](const MarketEvent&) {
        seen.push_back("event");
        ExecutionReport r; r.client_order_id = "x";
        bus.publishExecution(r);  // e.g. the mock exchange filling an order
        seen.push_back("after publish");
    });
    bus.publishMarketEvent(TradeTick{"A", 1, 1, 1, Side::BUY, {}});
    EXPECT_EQ(seen, (std::vector<std::string>{"event", "exec x", "after publish"}));
    bus.post([&] { seen.push_back("task"); });
    EXPECT_EQ(seen.back(), "task");
}
