#pragma once
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <unordered_map>
#include <set>
#include <string>
#include <mutex>
#include <optional>
#include <memory>

namespace bot {

class IExecutionEngine;

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
    size_t getTotalTrades() const;
    std::optional<Position> getPosition(const std::string& virtual_key) const;

    std::optional<double> getPortfolioValue() const;
    std::optional<double> getAvailableCash() const;

private:
    void syncWithExchange();

    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> engine_;
    std::unordered_map<std::string, Position> positions_;
    std::set<std::string> subscribedSymbols_; // Tracks active tick subscriptions
    mutable std::mutex mtx_;
    double total_realized_pnl_ = 0.0;
    size_t total_trades_{0};
};

} // namespace bot