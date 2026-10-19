#pragma once

#include <core/IEventBus.hpp>
#include <data/IDataFeed.hpp>
#include <execution/OrderManager.hpp>
#include <strategy/IStrategy.hpp>
#include <strategy/BasketState.hpp>
#include <risk/PortfolioManager.hpp>
#include <strategy/ExecutionCycle.hpp>
#include <strategy/BasketSizer.hpp>

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <map>
#include <chrono>
#include <unordered_map>

namespace bot {

enum class MasterState { 
    FLAT,               // Clean inventory, hunting for entry signals
    ENTRY_PENDING,      // Orders dispatched, waiting for exchange resolution
    IN_POSITION,        // Fully confirmed, balanced basket
    EXIT_PENDING,       // Closing orders dispatched, waiting for resolution
    RECOVERY_PENDING,   // enter recovery to try flaten
    SYSTEM_HALTED       // Fatal error state
};

class BasketMeanReversionStrategy : public IStrategy {
public:
    BasketMeanReversionStrategy(
        std::shared_ptr<IEventBus> eventBus,
        std::shared_ptr<OrderManager> orderMngr,
        std::shared_ptr<PortfolioManager> portfolioMngr,
        const std::string& configFilePath,
        double capitalAllocation);

    void start() override;
    void stop() override;

private:
    void loadConfig();
    void warmUp();
    void onMarketEvent(const MarketEvent& event);
    inline void processTick(const std::string& symbol, double price, uint64_t timestamp = 0);
    void executeBasket(PositionState targetState);
    void executeRecoverySweep(); // Recovery protocol
    void dispatchCycle(const std::vector<LegTarget>& targets, const char* label);
    bool hasInventory() const;
    void waitForCycle(std::chrono::milliseconds timeout) const;
    void onExecutionReport(const ExecutionReport& report);
    std::map<uint64_t, std::vector<double>> alignHistoricalBars(
        const std::vector<std::vector<HistoricalBar>>& all_bars, 
        size_t num_components) const;
    void evaluateCompletedCycle();
    bool isValidMarketEvent(const MarketEvent& event) const;
    inline void logZScoreThrottled(double z_score, uint64_t timestamp);

    std::shared_ptr<IEventBus> event_bus_;
    std::shared_ptr<OrderManager> order_mngr_;
    std::shared_ptr<PortfolioManager> portfolio_mngr_;
    std::unique_ptr<ExecutionCycle> current_cycle_;
    
    std::string config_path_;
    std::unique_ptr<BasketState> basket_state_;

    double entry_z_score_ = 2.0;
    double exit_z_score_ = 0.5;
    
    // Strict Deterministic State Flow
    MasterState master_state_ = MasterState::FLAT;
    PositionState current_position_ = PositionState::FLAT;
    
    // Virtual position key per leg ("<basket_id>_<SYMBOL>"). Orders get unique client IDs
    // derived from these (OrderIdUtils::newClientOrderId); positions are tracked under them.
    std::vector<std::string> position_keys_;

    static constexpr std::chrono::milliseconds kShutdownTimeout{10000};

    uint64_t report_subid_;
    std::unordered_map<std::string, uint64_t> market_subid_;

    std::string basket_id_;
    
    double capital_allocation_;

    uint64_t last_z_log_time_ms_ = 0;
};

} // namespace bot