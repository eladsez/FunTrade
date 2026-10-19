#include <gtest/gtest.h>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <execution/OrderManager.hpp>
#include <risk/PortfolioManager.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace bot;
using namespace bot::test;
namespace fs = std::filesystem;

// End-to-end: real strategy + OrderManager + PortfolioManager on a synchronous bus and a
// scripted engine. Basket is A - B; spread = log(A) - log(B).
class BasketStrategyTest : public ::testing::Test {
protected:
    static constexpr int kWindow = 20;
    static constexpr uint64_t kDayMs = 86'400'000;
    static constexpr uint64_t kT0 = 1'780'000'000'000;  // arbitrary epoch ms

    std::shared_ptr<SyncEventBus> bus = std::make_shared<SyncEventBus>();
    std::shared_ptr<FakeExecutionEngine> engine = std::make_shared<FakeExecutionEngine>();
    std::shared_ptr<OrderManager> om;
    std::shared_ptr<PortfolioManager> pm;
    std::shared_ptr<BasketMeanReversionStrategy> strat;
    fs::path cfgPath;
    uint64_t now = kT0 + 60 * kDayMs;
    std::map<std::string, double> last;

    void SetUp() override {
        engine->bus = bus.get();
        om = std::make_shared<OrderManager>(bus, engine);
        pm = std::make_shared<PortfolioManager>(bus, engine);

        cfgPath = fs::temp_directory_path() / ("funtrade_test_" + std::to_string(::getpid()) + ".json");
        std::ofstream(cfgPath) << R"({"basket_id":"tb","tickers":["A","B"],"weights":{"A":1.0,"B":-1.0},
            "trading_params":{"window_size":20,"entry_z":1.0,"exit_z":0.0}})";

        std::vector<HistoricalBar> a, b;
        for (int i = 0; i < 2 * kWindow; ++i) {
            uint64_t ts = kT0 + i * kDayMs;
            a.push_back(HistoricalBar{"A", ts, 0, 0, 0, 100.0 * std::exp(0.01 * std::sin(i * 0.7)), 1e6, 0, 0});
            b.push_back(HistoricalBar{"B", ts, 0, 0, 0, 100.0, 1e6, 0, 0});
        }
        bus->setHistory("A", a);
        bus->setHistory("B", b);

        strat = std::make_shared<BasketMeanReversionStrategy>(bus, om, pm, cfgPath.string(), 100000.0);
        strat->start();
    }

    void TearDown() override {
        // stop() flattens any open basket and waits for fills; fill instantly so it doesn't time out.
        engine->onPlace = [this](const OrderRequest& r) {
            engine->emit(FakeExecutionEngine::report(r, OrderStatus::FILLED, r.quantity, r.quantity, 100.0));
        };
        strat->stop();
        fs::remove(cfgPath);
    }

    // One synchronized round of midpoints at the given spread level.
    void tickSpread(double spread, int rounds = 1) {
        for (int i = 0; i < rounds; ++i) {
            now += 1000;
            last["A"] = 100.0 * std::exp(spread);
            last["B"] = 100.0;
            bus->publishMarketEvent(MidPointTick{"A", last["A"], now});
            bus->publishMarketEvent(MidPointTick{"B", last["B"], now});
        }
    }

    double px(const std::string& s) { return last.at(s); }
};

TEST_F(BasketStrategyTest, NoTradeWhileSpreadIsNormal) {
    tickSpread(0.0, 30);
    EXPECT_TRUE(engine->placed.empty());
}

TEST_F(BasketStrategyTest, ShortsTheSpreadWhenZBreachesEntry) {
    tickSpread(0.04, 30);
    ASSERT_EQ(engine->placed.size(), 2u) << "one order per leg, and no re-entry while pending";

    const auto& a = engine->placed[0].symbol == "A" ? engine->placed[0] : engine->placed[1];
    const auto& b = engine->placed[0].symbol == "B" ? engine->placed[0] : engine->placed[1];
    EXPECT_EQ(a.side, Side::SELL);
    EXPECT_EQ(b.side, Side::BUY);
    EXPECT_EQ(a.type, OrderType::MARKET);
    EXPECT_EQ(a.tif, TimeInForce::IOC);
    EXPECT_GT(a.quantity, 0);
    EXPECT_NEAR(a.quantity * px("A"), b.quantity * px("B"), 0.03 * a.quantity * px("A")) << "dollar neutral";
    engine->fillAll([this](const std::string& s) { return px(s); });
}

TEST_F(BasketStrategyTest, FullRoundTripEntryFillExitFill) {
    tickSpread(0.04, 30);
    ASSERT_EQ(engine->placed.size(), 2u);
    engine->fillAll([this](const std::string& s) { return px(s); });

    tickSpread(0.04, 5);
    EXPECT_EQ(engine->placed.size(), 2u) << "in position: no new orders while z stays high";

    tickSpread(-0.03, 40);
    ASSERT_EQ(engine->placed.size(), 4u) << "exit orders once z crosses exit threshold";
    std::map<std::string, OrderRequest> entries{{engine->placed[0].symbol, engine->placed[0]},
                                                {engine->placed[1].symbol, engine->placed[1]}};
    for (size_t i = 2; i < 4; ++i) {
        const auto& exit = engine->placed[i];
        const auto& entry = entries.at(exit.symbol);
        EXPECT_NE(exit.side, entry.side) << exit.symbol;
        EXPECT_DOUBLE_EQ(exit.quantity, entry.quantity) << exit.symbol;
    }
    engine->fillAll([this](const std::string& s) { return px(s); }, 2);

    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_A"), 0.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_B"), 0.0);
    EXPECT_GT(pm->getTotalRealizedPnL(), 0.0) << "shorted rich spread, covered cheap";
}

TEST_F(BasketStrategyTest, EntryWhiffRevertsToFlatAndCanReenter) {
    tickSpread(0.04, 30);
    ASSERT_EQ(engine->placed.size(), 2u);
    for (const auto& r : engine->placed)
        engine->emit(FakeExecutionEngine::report(r, OrderStatus::CANCELED, 0, 0, 0));

    tickSpread(0.04, 3);
    EXPECT_EQ(engine->placed.size(), 4u) << "back to FLAT, signal still on, so it re-enters";
    engine->fillAll([this](const std::string& s) { return px(s); }, 2);
}

TEST_F(BasketStrategyTest, PartialEntryTriggersRecoverySweepOfFilledInventory) {
    tickSpread(0.04, 30);
    ASSERT_EQ(engine->placed.size(), 2u);
    const auto r0 = engine->placed[0], r1 = engine->placed[1];
    engine->emit(FakeExecutionEngine::report(r0, OrderStatus::FILLED, r0.quantity, r0.quantity, px(r0.symbol)));
    engine->emit(FakeExecutionEngine::report(r1, OrderStatus::CANCELED, 0, 0, 0));

    ASSERT_EQ(engine->placed.size(), 3u) << "only the filled leg is unwound";
    EXPECT_EQ(engine->placed[2].symbol, r0.symbol);
    EXPECT_NE(engine->placed[2].side, r0.side);
    EXPECT_DOUBLE_EQ(engine->placed[2].quantity, r0.quantity);
}

// ---------------------------------------------------------------------------
// Known bugs (see docs/ROADMAP.md). Enable when the owning phase fixes them.
// ---------------------------------------------------------------------------

// Phase 4: the strategy only reacts to MidPointTick, which only the IBKR feed emits. The
// CSV backtest (trades + bars) and Alpaca (trades + quotes) can never trade.
TEST_F(BasketStrategyTest, DISABLED_Phase4_TradesOnQuoteStream) {
    for (int i = 0; i < 30; ++i) {
        now += 1000;
        double a = 100.0 * std::exp(0.04);
        bus->publishMarketEvent(QuoteTick{"A", a - 0.01, 100, a + 0.01, 100, now, {}});
        bus->publishMarketEvent(QuoteTick{"B", 99.99, 100, 100.01, 100, now, {}});
    }
    EXPECT_EQ(engine->placed.size(), 2u);
}

// Phase 2: stopping while entry orders are still working must cancel them and finish
// promptly. Today stop() builds an all-zero exit cycle (no inventory yet), never cancels the
// working entry orders, and blocks for its full 10 s timeout.
TEST_F(BasketStrategyTest, DISABLED_Phase2_StopWhileEntryPendingCancelsAndReturnsPromptly) {
    tickSpread(0.04, 30);
    ASSERT_EQ(engine->placed.size(), 2u);
    engine->onPlace = nullptr;
    auto t0 = std::chrono::steady_clock::now();
    strat->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    EXPECT_FALSE(engine->canceled.empty() && engine->cancelAllCalls == 0) << "working entry orders must be canceled";
}
