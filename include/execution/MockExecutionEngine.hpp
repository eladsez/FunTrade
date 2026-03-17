#pragma once

#include <execution/IExecutionEngine.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <chrono>

namespace bot {

class MockExecutionEngine : public IExecutionEngine {
public:
    // Inject the EventBus so the mock can simulate order fills back to the rest of the system
    explicit MockExecutionEngine(std::shared_ptr<IEventBus> eventBus) 
        : event_bus_(std::move(eventBus)) {}

    bool connectToInfra() override {
        LOG_INFO("[MOCK EXECUTION] Connected to Infra (Mock).");
        return true;
    }

    void disconnectFromInfra() override {
        LOG_INFO("[MOCK EXECUTION] Disconnected from Infra (Mock).");
    }

    std::string placeOrder(const OrderRequest& req) override {
        std::string side_str = (req.side == Side::BUY) ? "BUY" : "SELL";
        
        LOG_INFO("[MOCK EXECUTION] Placed Order: {} {:.4f} {} | Client ID: {}", 
                 side_str, req.quantity, req.symbol, req.client_order_id);

        // Simulate an immediate full execution of the order
        simulateFill(req);

        // Return a fake exchange-assigned order ID
        return generateClientOrderId(req.client_order_id);
    }

    bool cancelOrder(const std::string& orderId) override {
        LOG_INFO("[MOCK EXECUTION] Canceled order: {}", orderId);
        return true;
    }

    bool cancelAllOrders() override {
        LOG_INFO("[MOCK EXECUTION] Canceled all pending orders.");
        return true;
    }

    std::optional<double> getPortfolioValue() override {
        // Return a dummy starting paper-trading value
        return 100000.0;
    }

    // Add this mock implementation
    std::optional<double> getAvailableCash() override {
        return 100000.0; // Assume starting with 100k buying power
    }

    nlohmann::json getOpenPositions() override {
        // Return an empty array so PortfolioManager syncs cleanly on startup
        return nlohmann::json::array();
    }

    nlohmann::json getOpenOrders() override {
        // Return an empty array so OrderManager syncs cleanly on startup
        return nlohmann::json::array();
    }

private:
    std::shared_ptr<IEventBus> event_bus_;

    std::string generateClientOrderId(const std::string& prefix) {
        static std::atomic<uint32_t> order_counter{0};
        
        std::string base_prefix = prefix.empty() ? "order" : prefix;
        
        uint64_t timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        uint32_t current_count = ++order_counter;
        
        std::string generated_id = base_prefix + "_" + std::to_string(timestamp) + "_" + std::to_string(current_count);
        
        // Alpaca restricts client_order_id to a maximum of 48 characters
        if (generated_id.length() > 48) {
            generated_id = generated_id.substr(0, 48);
        }
        
        return generated_id;
    }

    void simulateFill(const OrderRequest& req) {
        if (!event_bus_) return;

        ExecutionReport report;
        // Mocking an exchange-assigned ID
        report.order_id = generateClientOrderId(req.client_order_id);
        report.client_order_id = generateClientOrderId(req.client_order_id);
        report.symbol = req.symbol;
        report.side = req.side;
        
        // Simulating a complete, instant fill
        report.status = OrderStatus::FILLED; 
        report.last_quantity = req.quantity;
        report.cumulative_quantity = req.quantity;
        
        // Using a safe placeholder price (100.0) so PortfolioManager math doesn't div-by-zero.
        report.last_price = 100.0; 
        report.avg_price = 100.0;
        
        // Generate current timestamp in milliseconds
        auto now = std::chrono::system_clock::now();
        report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        LOG_INFO("[MOCK EXECUTION] Simulating instant FILL for {}...", req.symbol);

        // Publish the execution report back to the event bus
        event_bus_->publishExecution(report); 
    }
};

} // namespace bot