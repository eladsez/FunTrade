#pragma once

#include <execution/IExecutionEngine.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>

namespace bot {

class MockExecutionEngine : public IExecutionEngine {
public:
    explicit MockExecutionEngine(std::shared_ptr<IEventBus> eventBus) 
        : event_bus_(std::move(eventBus)) {
        
        // Listen to ALL market events to maintain an up-to-date price cache
        event_bus_->subscribeToMarketEvents("*", [this](const MarketEvent& event) {
            std::visit([this](auto&& tick) {
                using T = std::decay_t<decltype(tick)>;
                if constexpr (std::is_same_v<T, TradeTick>) {
                    latest_prices_[tick.symbol] = tick.price;
                } else if constexpr (std::is_same_v<T, QuoteTick>) {
                    latest_prices_[tick.symbol] = (tick.bid_price + tick.ask_price) / 2.0;
                } else if constexpr (std::is_same_v<T, BarUpdate>) {
                    latest_prices_[tick.symbol] = tick.close;
                }
            }, event);
        });
    }

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

        return simulateFill(req);
    }

    bool cancelOrder(const std::string& orderId) override {
        LOG_INFO("[MOCK EXECUTION] Canceled order: {}", orderId);
        return true;
    }

    bool cancelAllOrders() override { return true; }
    std::optional<double> getPortfolioValue() override { return 100000.0; }
    std::optional<double> getAvailableCash() override { return 100000.0; }
    std::vector<Position> getOpenPositions() override { return std::vector<Position>(); }
    std::vector<OrderState> getOpenOrders() override { return std::vector<OrderState>(); }

private:
    std::shared_ptr<IEventBus> event_bus_;
    std::unordered_map<std::string, double> latest_prices_;

    std::string simulateFill(const OrderRequest& req) {
        if (!event_bus_) return "";

        static std::atomic<uint32_t> order_counter{0};

        ExecutionReport report;
        // Mocking an exchange-assigned ID
        report.order_id = order_counter++;
        report.client_order_id = req.client_order_id;
        report.symbol = req.symbol;
        report.side = req.side;
        
        // Simulating a complete, instant fill
        report.status = OrderStatus::FILLED; 
        report.last_quantity = req.quantity;
        report.cumulative_quantity = req.quantity;

        // Apply the real historical price from our cache
        double fill_price = latest_prices_.count(req.symbol) ? latest_prices_[req.symbol] : 0.0;
        
        if (fill_price == 0.0) {
            LOG_ERROR("[MOCK EXECUTION] FAILED to fill {}. No price data available in cache!", req.symbol);
            report.status = OrderStatus::REJECTED;
            report.last_price = 0.0;
            report.avg_price = 0.0;
        } else {
            report.last_price = fill_price;
            report.avg_price = fill_price;
        }

        // Apply the exact Virtual Time, not wall-clock
        report.timestamp_ms = std::chrono::system_clock::now().time_since_epoch().count();

        LOG_INFO("[MOCK EXECUTION] Simulating instant FILL for {} at ${:.2f}...", req.symbol, fill_price);
        event_bus_->publishExecution(report); 
        return report.order_id;
    }
};

} // namespace bot