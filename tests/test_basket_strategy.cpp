#include <gtest/gtest.h>
#include "fakes/StrategyHarness.hpp"
#include <chrono>
#include <fstream>
#include <map>

using namespace bot;
using namespace bot::test;

class BasketStrategyTest : public StrategyHarness {
protected:
    void SetUp() override { build(); start(); }
};

TEST_F(BasketStrategyTest, NoTradeWhileSpreadIsNormal) {
    for (int i = 0; i < 10; ++i) closeDay(0.4 * spreadAt(i));  // well inside the history's range
    EXPECT_TRUE(engine->placed.empty());
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);
}

TEST_F(BasketStrategyTest, SignalAtCloseTradesAtNextSessionWithMocOrders) {
    closeDay(0.05);
    EXPECT_TRUE(engine->placed.empty()) << "decided at the close, nothing sent yet";
    EXPECT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT);
    ASSERT_TRUE(strat->lastDailyZ());
    EXPECT_GT(*strat->lastDailyZ(), 1.0);

    openSession();  // first event of the next day
    ASSERT_EQ(engine->placed.size(), 2u);
    const auto& a = placedFor("A");
    const auto& b = placedFor("B");
    EXPECT_EQ(a.side, Side::SELL);
    EXPECT_EQ(b.side, Side::BUY);
    EXPECT_EQ(a.type, OrderType::MARKET_ON_CLOSE);
    EXPECT_EQ(a.tif, TimeInForce::DAY);
    EXPECT_NEAR(a.quantity * last["A"], b.quantity * last["B"], 0.03 * a.quantity * last["A"]) << "dollar neutral";
    EXPECT_EQ(strat->masterState(), MasterState::ENTRY_PENDING);
}

TEST_F(BasketStrategyTest, FullRoundTripEntryFillExitFill) {
    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 2u);
    fillFrom(0);
    EXPECT_EQ(strat->masterState(), MasterState::IN_POSITION);

    closeDay(0.05);
    openSession();
    EXPECT_EQ(engine->placed.size(), 2u) << "still rich: hold";

    closeDay(-0.02);
    EXPECT_EQ(strat->pendingAction(), PendingAction::EXIT);
    openSession();
    ASSERT_EQ(engine->placed.size(), 4u);
    std::map<std::string, OrderRequest> entries{{engine->placed[0].symbol, engine->placed[0]},
                                                {engine->placed[1].symbol, engine->placed[1]}};
    for (size_t i = 2; i < 4; ++i) {
        const auto& exit = engine->placed[i];
        EXPECT_NE(exit.side, entries.at(exit.symbol).side) << exit.symbol;
        EXPECT_DOUBLE_EQ(exit.quantity, entries.at(exit.symbol).quantity) << exit.symbol;
        EXPECT_EQ(exit.type, OrderType::MARKET_ON_CLOSE);
    }
    fillFrom(2);
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_A"), 0.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_B"), 0.0);
    EXPECT_GT(pm->getTotalRealizedPnL(), 0.0) << "shorted a rich spread, covered it cheap";
}

TEST_F(BasketStrategyTest, EntryWhiffRevertsToFlatAndRedecidesAtNextClose) {
    closeDay(0.05);
    openSession();
    for (const auto& r : engine->placed)
        engine->emit(FakeExecutionEngine::report(r, OrderStatus::CANCELED, 0, 0, 0));
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);

    closeDay(0.05);
    EXPECT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT);
    openSession();
    EXPECT_EQ(engine->placed.size(), 4u);
}

TEST_F(BasketStrategyTest, PartialEntrySchedulesRecoveryOfFilledLegOnly) {
    closeDay(0.05);
    openSession();
    const auto r0 = engine->placed[0], r1 = engine->placed[1];
    engine->emit(FakeExecutionEngine::report(r0, OrderStatus::FILLED, r0.quantity, r0.quantity, last[r0.symbol]));
    engine->emit(FakeExecutionEngine::report(r1, OrderStatus::CANCELED, 0, 0, 0));
    EXPECT_EQ(strat->masterState(), MasterState::RECOVERY_PENDING);
    EXPECT_EQ(strat->pendingAction(), PendingAction::RECOVER);
    EXPECT_EQ(engine->placed.size(), 2u) << "recovery waits for the next session (MOC)";

    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 3u);
    EXPECT_EQ(engine->placed[2].symbol, r0.symbol);
    EXPECT_NE(engine->placed[2].side, r0.side);
    EXPECT_DOUBLE_EQ(engine->placed[2].quantity, r0.quantity);
    fillFrom(2);
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);
}

TEST_F(BasketStrategyTest, FailedRecoveryHalts) {
    closeDay(0.05);
    openSession();
    const auto r0 = engine->placed[0], r1 = engine->placed[1];
    engine->emit(FakeExecutionEngine::report(r0, OrderStatus::FILLED, r0.quantity, r0.quantity, last[r0.symbol]));
    engine->emit(FakeExecutionEngine::report(r1, OrderStatus::CANCELED, 0, 0, 0));
    closeDay(0.0);
    openSession();
    ASSERT_EQ(engine->placed.size(), 3u);
    engine->emit(FakeExecutionEngine::report(engine->placed[2], OrderStatus::REJECTED, 0, 0, 0));
    EXPECT_EQ(strat->masterState(), MasterState::SYSTEM_HALTED);
    EXPECT_FALSE(strat->haltReason().empty());

    closeDay(0.05);
    openSession();
    EXPECT_EQ(engine->placed.size(), 3u) << "halted baskets never trade";
}

TEST_F(BasketStrategyTest, FailedPlacementResolvesCycle) {
    engine->failPlacements = true;
    closeDay(0.05);
    openSession();
    EXPECT_EQ(engine->placed.size(), 2u);
    EXPECT_EQ(strat->masterState(), MasterState::FLAT) << "placements failed -> whiff, not a hang";
    engine->failPlacements = false;
}

TEST_F(BasketStrategyTest, LateReportFromPreviousCycleIsIgnored) {
    closeDay(0.05);
    openSession();
    const auto oldA = placedFor("A");
    for (const auto& r : engine->placed)
        engine->emit(FakeExecutionEngine::report(r, OrderStatus::CANCELED, 0, 0, 0));  // whiff
    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 4u);
    EXPECT_NE(engine->placed[2].client_order_id, oldA.client_order_id);
    EXPECT_EQ(OrderIdUtils::positionKeyOf(engine->placed[2].client_order_id), "tb_A");

    engine->emit(FakeExecutionEngine::report(oldA, OrderStatus::FILLED, 0, oldA.quantity, last["A"]));
    EXPECT_EQ(strat->masterState(), MasterState::ENTRY_PENDING) << "stale report must not complete the new cycle";
    fillFrom(2);
    EXPECT_EQ(strat->masterState(), MasterState::IN_POSITION);
}

TEST_F(BasketStrategyTest, IntradayTicksGivePricesButNeverSignals) {
    for (int i = 0; i < 50; ++i) tick(0.08);  // wildly rich intraday
    EXPECT_TRUE(engine->placed.empty());
    EXPECT_EQ(strat->pendingAction(), PendingAction::NONE);
}

TEST_F(BasketStrategyTest, QuoteStreamStartsTheSessionToo) {
    closeDay(0.05);
    ASSERT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT);
    day++;
    bus->publishMarketEvent(QuoteTick{"A", 104.9, 100, 105.1, 100, ms(day) + 34'200'000, {}});
    EXPECT_EQ(engine->placed.size(), 2u);
}

TEST_F(BasketStrategyTest, WatchdogCancelsCycleUnresolvedAfterAFullSession) {
    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 2u);
    closeDay(0.05);  // same session's close: MOC fills may still be coming
    EXPECT_TRUE(engine->canceled.empty());
    closeDay(0.05);  // a full session later: stale
    EXPECT_EQ(engine->canceled.size(), 2u);
}

TEST_F(BasketStrategyTest, StopWhileEntryPendingCancelsAndReturnsPromptly) {
    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 2u);
    autoCancel();
    auto t0 = std::chrono::steady_clock::now();
    stopStrategy();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(1));
    EXPECT_EQ(engine->canceled.size(), 2u);
    EXPECT_EQ(engine->placed.size(), 2u) << "nothing filled: nothing to do";
}

TEST_F(BasketStrategyTest, StopInPositionDoesNotFlatten) {
    closeDay(0.05);
    openSession();
    fillFrom(0);
    stopStrategy();
    EXPECT_EQ(engine->placed.size(), 2u) << "positions persist across restarts; stop never trades";
    EXPECT_NE(pm->getNetPosition("tb_A"), 0.0);
}

// --- Risk ---

TEST_F(BasketStrategyTest, RiskGrossCapRejectsEntry) {
    rebuild([](RiskLimits& l) { l.max_gross_exposure_usd = 1000.0; });
    closeDay(0.05);
    openSession();
    EXPECT_TRUE(engine->placed.empty());
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);
}

TEST_F(BasketStrategyTest, KillSwitchBlocksEntriesButNotExits) {
    auto kill = tmp / "KILL";
    rebuild([&](RiskLimits& l) { l.kill_switch_file = kill.string(); });
    closeDay(0.05);
    openSession();
    fillFrom(0);
    std::ofstream(kill) << "";
    closeDay(-0.05);
    openSession();
    EXPECT_EQ(engine->placed.size(), 4u) << "exit still allowed";
    fillFrom(2);
    closeDay(0.06);
    openSession();
    EXPECT_EQ(engine->placed.size(), 4u) << "no new entry while the kill switch exists";
}

TEST_F(BasketStrategyTest, FlattenFileExitsAtNextSession) {
    auto flat = tmp / "FLATTEN";
    rebuild([&](RiskLimits& l) { l.flatten_file = flat.string(); });
    closeDay(0.05);
    openSession();
    fillFrom(0);
    std::ofstream(flat) << "";
    closeDay(0.05);  // still rich, but flatten requested
    EXPECT_EQ(strat->pendingAction(), PendingAction::EXIT);
}

TEST_F(BasketStrategyTest, BasketLossLimitExitsThenHalts) {
    rebuild([](RiskLimits& l) { l.max_basket_loss_usd = 100.0; });
    closeDay(0.05);
    openSession();
    fillFrom(0);
    closeDay(0.12);  // spread blows out further: the short loses
    EXPECT_EQ(strat->pendingAction(), PendingAction::EXIT);
    openSession();
    fillFrom(2);
    EXPECT_EQ(strat->masterState(), MasterState::SYSTEM_HALTED);
}

// Live mode re-reads the dividend-adjusted history at each close. That close is "today" on
// the wall clock and must be part of the rebuilt window, not dropped as a forming bar.
class DailyRebuildTest : public StrategyHarness {
protected:
    void setHistory(int64_t lastDay, double oldShift) {
        std::vector<HistoricalBar> a, b;
        for (int i = 0; i < 2 * kWindow; ++i) {
            int64_t d = lastDay - (2 * kWindow - 1) + i;
            double s = (i == 2 * kWindow - 1) ? 0.0 : oldShift + spreadAt(i);  // last day at 0
            a.push_back(HistoricalBar{"A", ms(d), 0, 0, 0, 100.0 * std::exp(s), 1e6, 0, 0});
            b.push_back(HistoricalBar{"B", ms(d), 0, 0, 0, 100.0, 1e6, 0, 0});
        }
        bus->setHistory("A", a);
        bus->setHistory("B", b);
    }
};

TEST_F(DailyRebuildTest, RebuildAtTodaysCloseUsesAdjustedHistory) {
    using namespace std::chrono;
    int64_t today = floor<days>(locate_zone("America/New_York")->to_local(system_clock::now()))
                        .time_since_epoch().count();
    rebuildDaily = true;
    day = today - 1;  // yesterday was the last close
    build();
    setHistory(today - 1, 0.0);
    start();
    EXPECT_EQ(strat->pendingAction(), PendingAction::NONE);

    // A dividend: the broker's adjusted history now has A's past closes lower.
    setHistory(today, -0.05);
    closeDay(0.0);  // today's close, at the unadjusted mean
    EXPECT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT) << "z must come from the adjusted window";
}
