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

// ---------------------------------------------------------------------------
// Known bugs (see docs/ROADMAP.md). Enable when the owning phase fixes them.
// ---------------------------------------------------------------------------

// Phase 2: a fill that arrives before submitOrder() records the order (fast exchange, or
// the mock engine) must not be overwritten back to NEW / 0 filled.
TEST_F(OrderManagerTest, DISABLED_Phase2_FillBeforeSubmitReturnsIsNotClobbered) {
    engine->onPlace = [this](const OrderRequest& r) {
        engine->emit(FakeExecutionEngine::report(r, OrderStatus::FILLED, r.quantity, r.quantity, 50.0));
    };
    std::string id = om->submitOrder(req("bk_A"));
    EXPECT_EQ(om->getOrder(id)->status, OrderStatus::FILLED);
    EXPECT_TRUE(om->getOpenOrders().empty());
}

// Phase 2: the id an engine returns from placeOrder() must be the id its reports carry
// (Alpaca returns the full generated id but reports the stripped base id).
TEST_F(OrderManagerTest, DISABLED_Phase2_ReturnedIdMatchesReportedId) {
    engine->onPlace = [this](const OrderRequest& r) {
        auto rep = FakeExecutionEngine::report(r, OrderStatus::FILLED, r.quantity, r.quantity, 50.0);
        rep.client_order_id = r.client_order_id + "_1792436188331276305_1";  // what Alpaca would key
        engine->emit(rep);
    };
    std::string id = om->submitOrder(req("bk_A"));
    EXPECT_EQ(om->getOrder(id)->status, OrderStatus::FILLED);
}
