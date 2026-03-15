#pragma once

#include <core/IEventBus.hpp>
#include <data/IDataFeed.hpp>
#include <execution/IExecutionEngine.hpp>
#include <strategy/IStrategy.hpp>
#include <strategy/BasketState.hpp>

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <map>

namespace bot {

class BasketMeanReversionStrategy : public IStrategy {
public:
    BasketMeanReversionStrategy(
        std::shared_ptr<IEventBus> eventBus,
        std::shared_ptr<IExecutionEngine> executionEngine,
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
    void onExecutionReport(const ExecutionReport& report);
    std::map<uint64_t, std::vector<double>> alignHistoricalBars(
        const std::vector<std::vector<HistoricalBar>>& all_bars, 
        size_t num_components) const;

    std::shared_ptr<IEventBus> event_bus_;
    std::shared_ptr<IExecutionEngine> exec_engine_;
    std::shared_ptr<PortfolioManager> portfolio_mngr_;
    
    std::string config_path_;
    std::unique_ptr<BasketState> basket_state_;

    double entry_z_score_ = 2.0;
    double exit_z_score_ = 0.5;
    
    // for now doesn't need a lock because it being changed and read only from one thread
    PositionState current_position_ = PositionState::FLAT;
    
    // Pre-allocated strings to avoid SSO allocations on the hot path
    std::vector<std::string> client_order_ids_;
    
    double capital_allocation_;
};

} // namespace bot