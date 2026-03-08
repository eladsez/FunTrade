#include <gtest/gtest.h>
#include <execution/OrderManager.hpp>
#include <core/EventBus.hpp>
#include <execution/DummyExecution.hpp>

using namespace bot;

class OrderManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::shared_ptr<DummyExecution> executionEngine;
    std::unique_ptr<OrderManager> orderManager;

    void SetUp() override {
        eventBus = std::make_shared<EventBus>();
        executionEngine = std::make_shared<DummyExecution>(*eventBus);
        orderManager = std::make_unique<OrderManager>(*eventBus, *executionEngine);
    }
};

TEST_F(OrderManagerTest, InitialStateIsEmpty) {
    EXPECT_TRUE(orderManager->getOpenOrders().empty());
}

TEST_F(OrderManagerTest, TrackNewOrder) {
    ExecutionReport report;
    report.order_id = "ex_id_1";
    report.client_order_id = "cl_id_1";
    report.symbol = "BTC/USD";
    report.side = Side::BUY;
    report.status = OrderStatus::NEW;
    report.timestamp_ms = 12345;

    eventBus->publishExecution(report);
    
    // Give time for event bus
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto openOrders = orderManager->getOpenOrders();
    ASSERT_EQ(openOrders.size(), 1);
    EXPECT_EQ(openOrders[0].client_order_id, "cl_id_1");
    EXPECT_EQ(openOrders[0].status, OrderStatus::NEW);
}

TEST_F(OrderManagerTest, OrderFilledRemovesFromOpen) {
    ExecutionReport report1;
    report1.order_id = "ex_id_1";
    report1.client_order_id = "cl_id_1";
    report1.status = OrderStatus::NEW;
    eventBus->publishExecution(report1);

    ExecutionReport report2;
    report2.order_id = "ex_id_1";
    report2.client_order_id = "cl_id_1";
    report2.status = OrderStatus::FILLED;
    eventBus->publishExecution(report2);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto openOrders = orderManager->getOpenOrders();
    EXPECT_TRUE(openOrders.empty());
    
    auto order = orderManager->getOrder("cl_id_1");
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->status, OrderStatus::FILLED);
}
