#include <gtest/gtest.h>
#include <execution/IbkrOrderTracker.hpp>
#include <core/Logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>

using namespace bot;

class IbkrOrderTrackerTest : public ::testing::Test {
protected:
    IbkrOrderTracker t;
    const std::string cid = "tb_A#1792436188331-1";

    void SetUp() override {
        OrderRequest r;
        r.client_order_id = cid; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
        t.onPlaced(42, r);
    }

    std::optional<ExecutionReport> exec(const std::string& id, double shares, double px = 50.0, long orderId = 42) {
        return t.onExecution(orderId, cid, id, "A", "BOT", shares, px, px);
    }
};

TEST_F(IbkrOrderTrackerTest, ExecutionIsAFullyInitialisedFill) {
    auto r = exec("e1", 10);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::FILLED);
    EXPECT_EQ(r->client_order_id, cid);
    EXPECT_EQ(r->order_id, "42");
    EXPECT_EQ(r->exec_id, "e1");
    EXPECT_EQ(r->symbol, "A");
    EXPECT_EQ(r->side, Side::BUY);
    EXPECT_DOUBLE_EQ(r->last_quantity, 10);
    EXPECT_DOUBLE_EQ(r->cumulative_quantity, 10);
    EXPECT_DOUBLE_EQ(r->last_price, 50.0);
}

TEST_F(IbkrOrderTrackerTest, OrderStatusNeverCarriesFillQuantity) {
    auto r = t.onOrderStatus(42, "Submitted", 0, 0);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::NEW);
    EXPECT_DOUBLE_EQ(r->last_quantity, 0);
    EXPECT_EQ(r->symbol, "A") << "fields come from the placed order, not left uninitialised";
    EXPECT_EQ(r->side, Side::BUY);
}

TEST_F(IbkrOrderTrackerTest, RepeatedOrderStatusIsReportedOnce) {
    EXPECT_TRUE(t.onOrderStatus(42, "Submitted", 0, 0));
    EXPECT_FALSE(t.onOrderStatus(42, "Submitted", 0, 0));
    EXPECT_FALSE(t.onOrderStatus(42, "PreSubmitted", 0, 0)) << "same domain status";
}

TEST_F(IbkrOrderTrackerTest, ExecThenFilledStatusGivesOneFillAndNoDuplicateTerminal) {
    auto f = exec("e1", 10);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, OrderStatus::FILLED);
    EXPECT_FALSE(t.onOrderStatus(42, "Filled", 10, 50.0));
    EXPECT_FALSE(t.onOrderStatus(42, "Filled", 10, 50.0));
}

TEST_F(IbkrOrderTrackerTest, FilledStatusBeforeExecutionIsHeldUntilExecutionArrives) {
    EXPECT_FALSE(t.onOrderStatus(42, "Filled", 10, 50.0)) << "positions would be incomplete";
    auto f = exec("e1", 10);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(f->last_quantity, 10);
}

TEST_F(IbkrOrderTrackerTest, IocPartialThenCancelKeepsFilledQuantity) {
    auto p = exec("e1", 4);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->status, OrderStatus::PARTIALLY_FILLED);

    auto c = t.onOrderStatus(42, "Cancelled", 4, 50.0);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->status, OrderStatus::CANCELED);
    EXPECT_DOUBLE_EQ(c->cumulative_quantity, 4);
    EXPECT_DOUBLE_EQ(c->last_quantity, 0);

    EXPECT_FALSE(t.onError(42, 202)) << "error 202 after the cancel is a duplicate";
}

TEST_F(IbkrOrderTrackerTest, CancelledStatusWithPendingExecutionsIsHeld) {
    EXPECT_FALSE(t.onOrderStatus(42, "Cancelled", 4, 50.0));
    auto f = exec("e1", 4);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->status, OrderStatus::CANCELED);
    EXPECT_DOUBLE_EQ(f->last_quantity, 4);
    EXPECT_DOUBLE_EQ(f->cumulative_quantity, 4);
}

TEST_F(IbkrOrderTrackerTest, Error202FirstThenCancelledStatusReportsOnce) {
    auto e = t.onError(42, 202);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->status, OrderStatus::CANCELED);
    EXPECT_FALSE(t.onOrderStatus(42, "Cancelled", 0, 0));
}

TEST_F(IbkrOrderTrackerTest, Error201IsRejection) {
    auto e = t.onError(42, 201);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->status, OrderStatus::REJECTED);
}

TEST_F(IbkrOrderTrackerTest, InformationalErrorsDoNotEndTheOrder) {
    EXPECT_FALSE(t.onError(42, 399));   // "order will not be placed until..." warning
    EXPECT_FALSE(t.onError(42, 2104));  // market data farm OK
    EXPECT_TRUE(t.onOrderStatus(42, "Submitted", 0, 0));
}

TEST_F(IbkrOrderTrackerTest, InactiveIsRejected) {
    auto r = t.onOrderStatus(42, "Inactive", 0, 0);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::REJECTED);
}

TEST_F(IbkrOrderTrackerTest, PendingCancelIsNotTerminal) {
    auto r = t.onOrderStatus(42, "PendingCancel", 0, 0);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::NEW);
    auto f = exec("e1", 10);
    ASSERT_TRUE(f) << "an order pending cancel can still fill";
    EXPECT_EQ(f->status, OrderStatus::FILLED);
}

TEST_F(IbkrOrderTrackerTest, DuplicateExecutionIsDropped) {
    EXPECT_TRUE(exec("e1", 4));
    EXPECT_FALSE(exec("e1", 4));
    auto r = exec("e2", 6);
    ASSERT_TRUE(r);
    EXPECT_DOUBLE_EQ(r->cumulative_quantity, 10);
}

TEST_F(IbkrOrderTrackerTest, StatusNeverClaimsMoreThanExecutionsSeen) {
    auto r = t.onOrderStatus(42, "Submitted", 6, 50.0);  // TWS says 6 filled, no exec yet
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(r->cumulative_quantity, 0);
}

TEST_F(IbkrOrderTrackerTest, LateExecutionAfterTerminalIsStillAFill) {
    ASSERT_TRUE(t.onError(42, 202));
    auto f = exec("e1", 3);
    ASSERT_TRUE(f);
    EXPECT_DOUBLE_EQ(f->last_quantity, 3);
    EXPECT_EQ(f->status, OrderStatus::CANCELED) << "terminal status is sticky";
}

TEST_F(IbkrOrderTrackerTest, ManualExecutionIsReportedOncePerRunNotPerReconnect) {
    // TWS replays the day's executions at every connect; RapidOrder (manual) trades logged
    // 8 warnings at each reconnect on 2026-10-20.
    auto sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(50);
    auto& logger = Logger::getLogger();
    auto level = logger->level();  // tests run with logging off (test_main.cpp)
    logger->set_level(spdlog::level::info);
    logger->sinks().push_back(sink);
    auto warnings = [&] {
        int n = 0;
        for (auto& l : sink->last_formatted()) n += l.find("untracked order") != std::string::npos;
        return n;
    };
    for (int connect = 0; connect < 3; ++connect) {
        EXPECT_FALSE(t.onExecution(0, "RapidOrder", "m1", "GWW", "BOT", 1, 900, 900));
        EXPECT_FALSE(t.onExecution(0, "RapidOrder", "m2", "AOS", "BOT", 23, 70, 70));
    }
    EXPECT_EQ(warnings(), 2) << "one per execution";
    logger->sinks().pop_back();
    logger->set_level(level);
}

TEST_F(IbkrOrderTrackerTest, UntrackedOrdersAreIgnored) {
    EXPECT_FALSE(t.onOrderStatus(7, "Filled", 1, 1));
    EXPECT_FALSE(t.onExecution(7, "manual", "x1", "Z", "BOT", 1, 1, 1));
    EXPECT_FALSE(t.onError(7, 201));
}

TEST_F(IbkrOrderTrackerTest, RestoredOrderFillsAreAttributedByOrderRef) {
    t.onOpenOrder(99, "tb_B#1792000000000-3", "B", Side::SELL, 5);
    auto f = t.onExecution(0, "tb_B#1792000000000-3", "e9", "B", "SLD", 5, 20.0, 20.0);  // orderId 0: other session
    ASSERT_TRUE(f);
    EXPECT_EQ(f->client_order_id, "tb_B#1792000000000-3");
    EXPECT_EQ(f->side, Side::SELL);
    EXPECT_EQ(f->status, OrderStatus::FILLED);
}

TEST_F(IbkrOrderTrackerTest, CancelLookupByClientId) {
    EXPECT_EQ(t.orderIdFor(cid), 42);
    EXPECT_FALSE(t.orderIdFor("nope"));
}
