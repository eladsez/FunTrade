#pragma once

#include "execution/IExecutionEngine.hpp"
#include "core/IEventBus.hpp"
#include "core/DomainModels.hpp"
#include <iostream>
#include <memory>
#include <string>
#include <chrono>

namespace bot {

class MockExecutionEngine : public IExecutionEngine {
public:
    // Inject the EventBus so the mock can simulate order fills back to the rest of the system
    explicit MockExecutionEngine(std::shared_ptr<IEventBus> eventBus) 
        : event_bus_(std::move(eventBus)) {}

    void placeOrder(const OrderRequest& req) override {
        std::string side_str = (req.side == Side::BUY) ? "BUY" : "SELL";
        
        // Log the order details to the console
        std::cout << "[MOCK EXECUTION] Placed Order: " 
                  << side_str << " " << req.quantity << " " 
                  << req.symbol << " | ID: " << req.client_order_id << "\n";

        // Simulate an immediate full execution of the order
        simulateFill(req);
    }

    void cancelAllOrders() override {
        std::cout << "[MOCK EXECUTION] Canceled all pending orders.\n";
    }

private:
    std::shared_ptr<IEventBus> event_bus_;

    void simulateFill(const OrderRequest& req) {
        if (!event_bus_) return;

        ExecutionReport report;
        // Mocking an exchange-assigned ID
        report.order_id = "MOCK_EXCH_" + req.client_order_id; 
        report.client_order_id = req.client_order_id;
        report.symbol = req.symbol;
        report.side = req.side;
        
        // Simulating a complete, instant fill
        report.status = OrderStatus::FILLED; 
        report.last_quantity = req.quantity;
        report.cumulative_quantity = req.quantity;
        
        // For a basic mock, we use a placeholder price. 
        // In an advanced mock, we might query the DataFeed for the current mid-price.
        report.last_price = 0.0; 
        report.avg_price = 0.0;
        
        // Generate current timestamp in milliseconds
        auto now = std::chrono::system_clock::now();
        report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        std::cout << "[MOCK EXECUTION] Simulating FILL for " << req.symbol << "...\n";

        // Publish the execution report back to the event bus
        event_bus_->publishExecution(report); 
    }
};

} // namespace bot