#pragma once
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <map>
#include <string>
#include <mutex>

namespace bot {

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
    PortfolioManager(IEventBus& eventBus);
    ~PortfolioManager() = default;

    // Callbacks for EventBus
    void onTick(const MarketTick& tick);
    void onExecution(const ExecutionReport& report);

    // Getters for risk checks and monitoring
    double getTotalRealizedPnL() const;
    double getTotalUnrealizedPnL() const;
    double getNetPosition(const std::string& symbol) const;
    Position getPosition(const std::string& symbol) const;

private:
    IEventBus& eventBus_;
    std::map<std::string, Position> positions_;
    mutable std::mutex mtx_;
    double total_realized_pnl_ = 0.0;
};

} // namespace bot
