#include <gtest/gtest.h>
#include <execution/OrderManager.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"

using namespace bot;
using namespace bot::test;

class OrderManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<SyncEventBus> bus = std::make_shared<SyncEventBus>();
    std::shared_ptr<FakeExecutionEngine> engine = std::make_shared<FakeExecutionEngine>();
    std::unique_ptr<OrderManager> om;

    void SetUp() override {
        engine->bus = bus.get();
        om = std::make_unique<OrderManager>(bus, engine);
    }

    static OrderRequest req(const std::string& cid, double qty = 10) {
        OrderRequest r{};
        r.client_order_id = cid; r.symbol = "A"; r.side = Side::BUY;
        r.type = OrderType::MARKET; r.tif = TimeInForce::IOC; r.quantity = qty;
        return r;
    }
};

TEST_F(OrderManagerTest, SubmitTracksOrderAsNew) {
    std::string id = om->submitOrder(req("bk_A"));
    ASSERT_EQ(engine->placed.size(), 1u);
    auto st = om->getOrder(id);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->status, OrderStatus::NEW);
    EXPECT_DOUBLE_EQ(st->quantity, 10);
    EXPECT_EQ(om->getOpenOrders().size(), 1u);
}

TEST_F(OrderManagerTest, FillUpdatesStateAndClosesOrder) {
    auto r = req("bk_A");
    std::string id = om->submitOrder(r);
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::PARTIALLY_FILLED, 4, 4, 50.0));
    EXPECT_DOUBLE_EQ(om->getOrder(id)->filled_quantity, 4);
    EXPECT_EQ(om->getOpenOrders().size(), 1u);

    engine->emit(FakeExecutionEngine::report(r, OrderStatus::FILLED, 6, 10, 50.0));
    EXPECT_EQ(om->getOrder(id)->status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(om->getOrder(id)->filled_quantity, 10);
    EXPECT_TRUE(om->getOpenOrders().empty());
}

TEST_F(OrderManagerTest, CancelGoesToEngineByExchangeId) {
    auto r = req("bk_A");
    std::string id = om->submitOrder(r);
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::NEW, 0, 0, 0.0, "EX-1"));
    om->cancelOrder(id);
    ASSERT_EQ(engine->canceled.size(), 1u);
    EXPECT_EQ(engine->canceled[0], "EX-1");
}

TEST_F(OrderManagerTest, LoadsBrokerOpenOrdersOnStartup) {
    auto e = std::make_shared<FakeExecutionEngine>();
    OrderState s{};
    s.order_id = "EX-9"; s.client_order_id = "bk_Z"; s.symbol = "Z"; s.status = OrderStatus::NEW;
    e->openOrders = {s};
    OrderManager om2(bus, e);
    EXPECT_TRUE(om2.getOrder("bk_Z").has_value());
}

// Fixed in Phase 2 (were known bugs).

// A fill that arrives before submitOrder() returns (fast exchange, or the mock engine)
// must not be overwritten back to NEW / 0 filled.
TEST_F(OrderManagerTest, FillBeforeSubmitReturnsIsNotClobbered) {
    engine->onPlace = [this](const OrderRequest& r) {
        engine->emit(FakeExecutionEngine::report(r, OrderStatus::FILLED, r.quantity, r.quantity, 50.0));
    };
    std::string id = om->submitOrder(req("bk_A"));
    EXPECT_EQ(om->getOrder(id)->status, OrderStatus::FILLED);
    EXPECT_DOUBLE_EQ(om->getOrder(id)->filled_quantity, 10);
    EXPECT_TRUE(om->getOpenOrders().empty());
}

// An engine that fails to place (returns a different/empty id) yields a REJECTED order and "".
TEST_F(OrderManagerTest, FailedPlacementIsRejected) {
    struct FailingEngine : FakeExecutionEngine {
        std::string placeOrder(const OrderRequest& r) override { placed.push_back(r); return ""; }
    };
    auto failing = std::make_shared<FailingEngine>();
    OrderManager om2(bus, failing);
    EXPECT_EQ(om2.submitOrder(req("bk_A")), "");
    EXPECT_EQ(om2.getOrder("bk_A")->status, OrderStatus::REJECTED);
    EXPECT_TRUE(om2.getOpenOrders().empty());
}

TEST_F(OrderManagerTest, TerminalStatusIsFinalAndFillsOnlyGrow) {
    auto r = req("bk_A");
    std::string id = om->submitOrder(r);
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::PARTIALLY_FILLED, 6, 6, 50.0));
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::CANCELED, 0, 0, 0.0));
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::NEW, 0, 0, 0.0));  // late
    EXPECT_EQ(om->getOrder(id)->status, OrderStatus::CANCELED);
    EXPECT_DOUBLE_EQ(om->getOrder(id)->filled_quantity, 6);
}

TEST_F(OrderManagerTest, CancelBeforeFirstReportUsesClientId) {
    std::string id = om->submitOrder(req("bk_A"));
    om->cancelOrder(id);
    ASSERT_EQ(engine->canceled.size(), 1u);
    EXPECT_EQ(engine->canceled[0], id);
}

TEST_F(OrderManagerTest, CancelOfTerminalOrderIsNoop) {
    auto r = req("bk_A");
    std::string id = om->submitOrder(r);
    engine->emit(FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0));
    om->cancelOrder(id);
    EXPECT_TRUE(engine->canceled.empty());
}
