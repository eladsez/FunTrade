#include <gtest/gtest.h>
#include <strategy/ExecutionCycle.hpp>

using namespace bot;

namespace {

ExecutionReport rep(const std::string& cid, OrderStatus st, double cum) {
    ExecutionReport r{};
    r.client_order_id = cid;
    r.status = st;
    r.cumulative_quantity = cum;
    return r;
}

} // namespace

TEST(ExecutionCycle, CompletesWhenEveryLegFills) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 10, Side::BUY);
    c.addLeg("bk_B", "B", 5, Side::SELL);

    EXPECT_FALSE(c.processExecutionReport(rep("bk_A", OrderStatus::FILLED, 10)));
    EXPECT_TRUE(c.processExecutionReport(rep("bk_B", OrderStatus::FILLED, 5)));
    EXPECT_TRUE(c.hasAnyFills());
    EXPECT_DOUBLE_EQ(c.getMissedShares(), 0.0);
}

TEST(ExecutionCycle, PartialThenCancelIsTerminalWithMissedShares) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 100, Side::BUY);
    EXPECT_FALSE(c.processExecutionReport(rep("bk_A", OrderStatus::PARTIALLY_FILLED, 40)));
    EXPECT_TRUE(c.processExecutionReport(rep("bk_A", OrderStatus::CANCELED, 40)));
    EXPECT_DOUBLE_EQ(c.getMissedShares(), 60.0);
}

TEST(ExecutionCycle, RejectWithNoFillsIsAWhiff) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 100, Side::BUY);
    EXPECT_TRUE(c.processExecutionReport(rep("bk_A", OrderStatus::REJECTED, 0)));
    EXPECT_FALSE(c.hasAnyFills());
}

TEST(ExecutionCycle, IgnoresReportsForOtherOrders) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 10, Side::BUY);
    EXPECT_FALSE(c.processExecutionReport(rep("other_A", OrderStatus::FILLED, 10)));
    EXPECT_FALSE(c.isComplete());
}

TEST(ExecutionCycle, ZeroTargetLegsDoNotBlockCompletion) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 0, Side::BUY);
    c.addLeg("bk_B", "B", 5, Side::SELL);
    EXPECT_TRUE(c.processExecutionReport(rep("bk_B", OrderStatus::FILLED, 5)));
}

// Fixed in Phase 2 (were known bugs).

// An exit with nothing to trade must complete immediately, or the basket
// sits in EXIT_PENDING forever.
TEST(ExecutionCycle, AllZeroTargetsCompleteImmediately) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 0, Side::BUY);
    c.addLeg("bk_B", "B", 0, Side::SELL);
    EXPECT_TRUE(c.isComplete());
    EXPECT_FALSE(c.hasLegs());
}

// IBKR's error-202 cancel report carries cumulative_quantity = 0. A later report
// must never erase shares that were already filled.
TEST(ExecutionCycle, CancelReportDoesNotEraseEarlierFills) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 100, Side::BUY);
    c.processExecutionReport(rep("bk_A", OrderStatus::PARTIALLY_FILLED, 60));
    c.processExecutionReport(rep("bk_A", OrderStatus::CANCELED, 0));
    EXPECT_TRUE(c.hasAnyFills());
    EXPECT_DOUBLE_EQ(c.getMissedShares(), 40.0);
}

// Once a leg is terminal, a late/duplicate report must not reopen it.
TEST(ExecutionCycle, TerminalLegIgnoresLateReports) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 10, Side::BUY);
    c.processExecutionReport(rep("bk_A", OrderStatus::FILLED, 10));
    c.processExecutionReport(rep("bk_A", OrderStatus::NEW, 0));
    EXPECT_TRUE(c.isComplete());
    EXPECT_DOUBLE_EQ(c.getMissedShares(), 0.0);
}

TEST(ExecutionCycle, WorkingIdsListOnlyNonTerminalLegs) {
    ExecutionCycle c("bk");
    c.addLeg("bk_A", "A", 10, Side::BUY);
    c.addLeg("bk_B", "B", 10, Side::SELL);
    c.processExecutionReport(rep("bk_A", OrderStatus::FILLED, 10));
    EXPECT_EQ(c.workingClientOrderIds(), std::vector<std::string>{"bk_B"});
}
