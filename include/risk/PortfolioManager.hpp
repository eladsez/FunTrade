#pragma once
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <optional>
#include <memory>

namespace bot {

class IExecutionEngine;

struct Position {
    std::string symbol;
    double quantity = 0.0;
    double avg_price = 0.0;
    double current_price = 0.0;
    double realized_pnl = 0.0;

    double unrealized_pnl() const {
        if (quantity == 0.0) return 0.0;
        return quantity * (current_price - avg_price);
    }
};

class PortfolioManager {
public:
    // Updated: Pass the engine into the constructor
    PortfolioManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine);
    ~PortfolioManager() = default;

    void onMarketEvent(const MarketEvent& event);
    void onExecution(const ExecutionReport& report);

    double getTotalRealizedPnL() const;
    double getTotalUnrealizedPnL() const;
    double getNetPosition(const std::string& virtual_key) const;
    std::optional<Position> getPosition(const std::string& virtual_key) const;

    std::optional<double> getPortfolioValue() const;
    std::optional<double> getAvailableCash() const;

private:
    void syncWithExchange();
    
    // Helper to dynamically subscribe to ticks without duplicating
    void ensureMarketSubscription(const std::string& symbol);

    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> engine_;
    std::map<std::string, Position> positions_;
    std::set<std::string> subscribedSymbols_; // Tracks active tick subscriptions
    mutable std::mutex mtx_;
    double total_realized_pnl_ = 0.0;
};

} // namespace bot