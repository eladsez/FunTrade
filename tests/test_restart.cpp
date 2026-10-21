#include <gtest/gtest.h>
#include "fakes/StrategyHarness.hpp"
#include <fstream>

using namespace bot;
using namespace bot::test;

// Restart behaviour: a new bot instance on the same state files and the same broker
// account (the fake engine is kept) must resume exactly, or halt when it can't be sure.
class RestartTest : public StrategyHarness {
protected:
    void SetUp() override {
        persist = true;
        build();
        start();
    }

    // Graceful restart: stop, then build a new bot on the same state + broker.
    void restart() {
        stopStrategy();
        crashRestart();
    }

    // Crash: the old objects simply vanish (no stop(), nothing flushed beyond what was
    // already persisted), then a new bot starts.
    void crashRestart() {
        stopped = true;   // the old instance is abandoned
        strat.reset();
        engine->placed.clear();
        engine->canceled.clear();
        build(/*keepEngine=*/true);
        start();
    }
};

TEST_F(RestartTest, InPositionSurvivesRestartAndExitsLater) {
    closeDay(0.05);
    openSession();
    fillFrom(0);
    double qa = pm->getNetPosition("tb_A");
    ASSERT_NE(qa, 0.0);

    restart();
    EXPECT_EQ(strat->masterState(), MasterState::IN_POSITION);
    EXPECT_EQ(strat->position(), PositionState::SHORT);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_A"), qa) << "virtual books restored from disk";

    closeDay(-0.05);
    EXPECT_EQ(strat->pendingAction(), PendingAction::EXIT);
    openSession();
    ASSERT_EQ(engine->placed.size(), 2u);
    fillFrom(0);
    EXPECT_EQ(strat->masterState(), MasterState::FLAT);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_A"), 0.0);
}

TEST_F(RestartTest, PendingDecisionSurvivesRestartAndRunsOnce) {
    closeDay(0.05);
    ASSERT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT);
    restart();
    EXPECT_EQ(strat->pendingAction(), PendingAction::ENTER_SHORT);
    openSession();
    EXPECT_EQ(engine->placed.size(), 2u);

    // Restart again mid-session after the orders went out: no duplicate orders.
    crashRestart();
    openSession();
    EXPECT_TRUE(engine->placed.empty());
}

TEST_F(RestartTest, CrashMidCycleReattachesWorkingOrders) {
    closeDay(0.05);
    openSession();
    ASSERT_EQ(engine->placed.size(), 2u);
    const auto a = placedFor("A"), b = placedFor("B");
    engine->emit(FakeExecutionEngine::report(a, OrderStatus::FILLED, a.quantity, a.quantity, last["A"]));

    // B's MOC order is still working at the broker when the bot dies.
    OrderState working;
    working.order_id = "X-" + b.client_order_id;
    working.client_order_id = b.client_order_id;
    working.symbol = "B";
    working.side = b.side;
    working.quantity = b.quantity;
    working.status = OrderStatus::NEW;
    engine->openOrders = {working};

    crashRestart();
    EXPECT_EQ(strat->masterState(), MasterState::ENTRY_PENDING) << "cycle reattached, not guessed";

    engine->openOrders.clear();
    engine->emit(FakeExecutionEngine::report(b, OrderStatus::FILLED, b.quantity, b.quantity, last["B"]));
    EXPECT_EQ(strat->masterState(), MasterState::IN_POSITION);
}

TEST_F(RestartTest, CrashAfterPartialFillWithNothingWorkingSchedulesRecovery) {
    closeDay(0.05);
    openSession();
    const auto a = placedFor("A");
    engine->emit(FakeExecutionEngine::report(a, OrderStatus::FILLED, a.quantity, a.quantity, last["A"]));
    // B's order died while the bot was down (e.g. MOC cutoff): nothing is working.
    crashRestart();
    EXPECT_EQ(strat->masterState(), MasterState::RECOVERY_PENDING);
    EXPECT_EQ(strat->pendingAction(), PendingAction::RECOVER);
}

TEST_F(RestartTest, BrokerMismatchHaltsTheBasket) {
    closeDay(0.05);
    openSession();
    fillFrom(0);
    stopStrategy();
    // Someone trades A manually while the bot is down.
    engine->openPositions[0].quantity += 7;
    crashRestart();
    EXPECT_EQ(strat->masterState(), MasterState::SYSTEM_HALTED);
    EXPECT_NE(strat->haltReason().find("A"), std::string::npos);

    closeDay(-0.05);
    openSession();
    EXPECT_TRUE(engine->placed.empty()) << "halted: no trading on unknown inventory";
}

TEST_F(RestartTest, HaltPersistsAcrossRestarts) {
    closeDay(0.05);
    openSession();
    fillFrom(0);
    stopStrategy();
    engine->openPositions[0].quantity += 7;
    crashRestart();
    ASSERT_EQ(strat->masterState(), MasterState::SYSTEM_HALTED);
    engine->openPositions[0].quantity -= 7;  // fixed at the broker, but a human must clear the halt
    restart();
    EXPECT_EQ(strat->masterState(), MasterState::SYSTEM_HALTED);
}

TEST_F(RestartTest, ExecutionRedeliveredAfterRestartIsNotBookedTwice) {
    closeDay(0.05);
    openSession();
    const auto a = placedFor("A");
    auto fill = FakeExecutionEngine::report(a, OrderStatus::FILLED, a.quantity, a.quantity, last["A"], "", "EXEC-1");
    engine->emit(fill);
    double q = pm->getNetPosition("tb_A");
    crashRestart();
    engine->trackPositions = false;  // the broker already has it; IBKR merely replays the execution
    bus->publishExecution(fill);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("tb_A"), q);
}

TEST_F(RestartTest, CorruptStateFileRefusesToStart) {
    stopStrategy();
    std::ofstream(tmp / "baskets" / "tb.json") << "{ not json";
    strat.reset();
    build(true);
    EXPECT_ANY_THROW(start());
    stopped = true;
}

// The entry record (display only) is saved with the position and dropped once flat.
TEST_F(RestartTest, EntryRecordSurvivesRestartAndClearsWhenFlat) {
    closeDay(0.05);
    const int64_t decided = day;
    const double z = strat->lastDailyZ().value();
    openSession();
    fillFrom(0);
    ASSERT_EQ(strat->masterState(), MasterState::IN_POSITION);

    restart();
    ASSERT_TRUE(strat->entry());
    EXPECT_EQ(strat->entry()->decision_day, decided);
    EXPECT_EQ(strat->entry()->day, decided + 1) << "MOC entry sent (and filled) the next session";
    EXPECT_DOUBLE_EQ(strat->entry()->z.value(), z);
    EXPECT_FALSE(strat->entry()->restored);

    closeDay(-0.05);
    openSession();
    fillFrom(0);
    ASSERT_EQ(strat->masterState(), MasterState::FLAT);
    auto j = nlohmann::json::parse(std::ifstream(tmp / "baskets" / "tb.json"));
    EXPECT_TRUE(j.at("entry").is_null());
    restart();
    EXPECT_FALSE(strat->entry());
}

// Positions saved before the record existed: rebuilt from cycle_day and the close before it.
TEST_F(RestartTest, LegacyPositionGetsItsEntryRestored) {
    closeDay(0.05);
    const int64_t decided = day;
    openSession();
    fillFrom(0);
    stopStrategy();
    auto path = tmp / "baskets" / "tb.json";
    auto j = nlohmann::json::parse(std::ifstream(path));
    j.erase("entry");
    std::ofstream(path) << j.dump();

    crashRestart();
    ASSERT_EQ(strat->masterState(), MasterState::IN_POSITION);
    ASSERT_TRUE(strat->entry());
    EXPECT_EQ(strat->entry()->decision_day, decided);
    EXPECT_EQ(strat->entry()->day, decided + 1);
    EXPECT_TRUE(strat->entry()->restored);
    auto saved = nlohmann::json::parse(std::ifstream(path));
    EXPECT_EQ(saved.at("entry").at("decision_day"), decided) << "persisted, so it's restored only once";
    EXPECT_EQ(saved.at("entry").at("restored"), true);
}
