#include <gtest/gtest.h>
#include <risk/PortfolioManager.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"
#include <filesystem>
#include <core/Logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <unistd.h>

using namespace bot;
using namespace bot::test;

class PortfolioManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<SyncEventBus> bus = std::make_shared<SyncEventBus>();
    std::shared_ptr<FakeExecutionEngine> engine = std::make_shared<FakeExecutionEngine>();
    std::unique_ptr<PortfolioManager> pm;

    void SetUp() override {
        engine->trackPositions = false;  // these tests set broker positions explicitly
        pm = std::make_unique<PortfolioManager>(bus, engine);
    }

    void fill(const std::string& vkey, const std::string& sym, Side side, double qty, double px,
              OrderStatus st = OrderStatus::FILLED) {
        OrderRequest r{};
        r.client_order_id = vkey; r.symbol = sym; r.side = side; r.quantity = qty;
        bus->publishExecution(FakeExecutionEngine::report(r, st, qty, qty, px));
    }
};

TEST_F(PortfolioManagerTest, OpensLongAndShort) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_B", "B", Side::SELL, 4, 20.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_B"), -4);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 50.0);
}

TEST_F(PortfolioManagerTest, AddingUpdatesVwap) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::BUY, 10, 60.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 20);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 55.0);
}

TEST_F(PortfolioManagerTest, ReducingRealizesPnl) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::SELL, 4, 55.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 6);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->realized_pnl, 20.0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 20.0);
}

TEST_F(PortfolioManagerTest, ShortCoverRealizesPnl) {
    fill("bk_A", "A", Side::SELL, 10, 50.0);
    fill("bk_A", "A", Side::BUY, 10, 45.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 50.0);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->realized_pnl, 50.0) << "flat positions keep their realized PnL";
    EXPECT_DOUBLE_EQ(pm->getPnL({"bk_A"}), 50.0);
}

TEST_F(PortfolioManagerTest, FlipResetsAveragePrice) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::SELL, 15, 52.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), -5);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 52.0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 20.0);
}

TEST_F(PortfolioManagerTest, VirtualKeysSharingASymbolAreIndependent) {
    fill("bk1_HBAN", "HBAN", Side::BUY, 100, 15.0);
    fill("bk2_HBAN", "HBAN", Side::SELL, 60, 15.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk1_HBAN"), 100);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk2_HBAN"), -60);

    bus->publishMarketEvent(TradeTick{"HBAN", 16.0, 1, 0, Side::BUY, {}});
    EXPECT_DOUBLE_EQ(pm->getPosition("bk1_HBAN")->unrealized_pnl(), 100.0);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk2_HBAN")->unrealized_pnl(), -60.0);
    EXPECT_DOUBLE_EQ(pm->getTotalUnrealizedPnL(), 40.0);
}

TEST_F(PortfolioManagerTest, StatusOnlyReportsNeverMoveInventory) {
    // IBKR orderStatus says "Filled, cumulative 10" but carries no execution.
    OrderRequest r{};
    r.client_order_id = "bk_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    for (auto st : {OrderStatus::NEW, OrderStatus::PARTIALLY_FILLED, OrderStatus::FILLED,
                    OrderStatus::CANCELED, OrderStatus::REJECTED}) {
        bus->publishExecution(FakeExecutionEngine::report(r, st, 0, 10, 50.0));
    }
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 0);
}

TEST_F(PortfolioManagerTest, LateFillAfterCancelIsStillBooked) {
    fill("bk_A", "A", Side::BUY, 10, 50.0, OrderStatus::CANCELED);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
}

TEST_F(PortfolioManagerTest, BooksUnderPositionKeyOfUniqueOrderId) {
    fill(OrderIdUtils::newClientOrderId("bk_A"), "A", Side::BUY, 10, 50.0);
    fill(OrderIdUtils::newClientOrderId("bk_A"), "A", Side::BUY, 5, 50.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 15);
}

TEST_F(PortfolioManagerTest, BrokerPositionsAreKeptApartFromVirtualBooks) {
    auto e = std::make_shared<FakeExecutionEngine>();
    e->openPositions = {Position{"AAPL", 5, 180.0, 185.0, 0.0}};
    PortfolioManager pm2(bus, e);
    EXPECT_DOUBLE_EQ(pm2.getBrokerPosition("AAPL"), 5);
    EXPECT_DOUBLE_EQ(pm2.getNetPosition("AAPL"), 0) << "not the bot's position";
    EXPECT_FALSE(pm2.isReconciled("AAPL"));
    auto m = pm2.reconcile();
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].symbol, "AAPL");
}

TEST_F(PortfolioManagerTest, ReconcilesSumOfBasketsAgainstBroker) {
    fill("bk1_HBAN", "HBAN", Side::BUY, 100, 15.0);
    fill("bk2_HBAN", "HBAN", Side::SELL, 60, 15.0);
    engine->openPositions = {Position{"HBAN", 40, 15.0, 15.0, 0.0}};
    pm->resync();
    EXPECT_TRUE(pm->isReconciled("HBAN"));
    engine->openPositions = {Position{"HBAN", 41, 15.0, 15.0, 0.0}};
    pm->resync();
    EXPECT_FALSE(pm->isReconciled("HBAN"));
}

TEST_F(PortfolioManagerTest, ExposureAggregates) {
    fill("bk1_A", "A", Side::BUY, 10, 50.0);
    fill("bk2_A", "A", Side::SELL, 4, 50.0);
    EXPECT_DOUBLE_EQ(pm->getNetSymbolQuantity("A"), 6);
    EXPECT_DOUBLE_EQ(pm->getGrossExposure(), 700.0);
}

// The same execution delivered twice must be counted once (fixed in Phase 2).
TEST_F(PortfolioManagerTest, DuplicateExecutionCountedOnce) {
    OrderRequest r{};
    r.client_order_id = "bk_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    auto rep = FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0);
    bus->publishExecution(rep);
    bus->publishExecution(rep);  // same exec_id, redelivered
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
    EXPECT_EQ(pm->getTotalTrades(), 1u);
}

// Fixed in Phase 3 (was a known bug): per-basket books are persisted and restored, so
// inventory stays attributed to the basket that owns it across restarts.
TEST_F(PortfolioManagerTest, RestoredPositionsAreAttributedToBaskets) {
    auto file = std::filesystem::temp_directory_path() / ("funtrade_pm_" + std::to_string(::getpid()) + ".json");
    std::filesystem::remove(file);
    {
        PortfolioManager pm1(bus, engine, file.string());
        OrderRequest r{};
        r.client_order_id = OrderIdUtils::newClientOrderId("bk_A"); r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
        bus->publishExecution(FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0));
    }
    engine->openPositions = {Position{"A", 10, 50.0, 50.0, 0.0}};
    auto bus2 = std::make_shared<SyncEventBus>();
    PortfolioManager pm2(bus2, engine, file.string());
    EXPECT_DOUBLE_EQ(pm2.getNetPosition("bk_A"), 10);
    EXPECT_DOUBLE_EQ(pm2.getPosition("bk_A")->avg_price, 50.0);
    EXPECT_TRUE(pm2.isReconciled("A"));
    std::filesystem::remove(file);
}

// Live, the PM is built before the broker is connected. Its first snapshot is empty, so a
// reconciliation report at construction listed every held leg as a MISMATCH (2026-10-20).
TEST_F(PortfolioManagerTest, NoReconciliationReportBeforeTheBrokerIsSynced) {
    auto file = std::filesystem::temp_directory_path() / ("pm_recon_" + std::to_string(::getpid()) + ".json");
    PortfolioManager writer(bus, engine, file.string());
    writer.onExecution([] {
        ExecutionReport r;
        r.client_order_id = OrderIdUtils::newClientOrderId("bk_A");
        r.symbol = "A"; r.side = Side::BUY; r.last_quantity = 10; r.last_price = 50.0; r.exec_id = "x1";
        return r;
    }());

    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(200);
    auto& logger = Logger::getLogger();
    auto level = logger->level();  // tests run with logging off (test_main.cpp)
    logger->set_level(spdlog::level::info);
    logger->sinks().push_back(sink);
    auto unsynced = std::make_shared<FakeExecutionEngine>();  // broker not connected yet: no positions
    PortfolioManager pm2(bus, unsynced, file.string());
    auto logged = [&] { std::string all; for (auto& l : sink->last_formatted()) all += l; return all; };
    EXPECT_EQ(logged().find("MISMATCH"), std::string::npos) << logged();

    pm2.resync();  // an explicit resync still reports a real mismatch
    EXPECT_NE(logged().find("MISMATCH A"), std::string::npos);

    unsynced->openPositions = {Position{"A", 10, 50.0, 50.0, 0.0}};
    pm2.resync();  // main, after the account sync
    EXPECT_TRUE(pm2.isReconciled("A"));
    logger->sinks().pop_back();
    logger->set_level(level);
    std::filesystem::remove(file);
}
