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

namespace bot {

class BasketMeanReversionStrategy : public IStrategy {
public:
    BasketMeanReversionStrategy(
        std::shared_ptr<IEventBus> eventBus,
        std::shared_ptr<IExecutionEngine> executionEngine,
        const std::string& configFilePath);

    void start() override;
    void stop() override;
    
    // Explicitly call this from a timer or daily event generator
    void onBarClose(); 

private:
    void loadConfig();
    void warmUp();
    void onMarketEvent(const MarketEvent& event);
    inline void processTick(const std::string& symbol, double price, uint64_t timestamp = 0);
    void executeBasket(PositionState targetState);

    std::shared_ptr<IEventBus> event_bus_;
    std::shared_ptr<IExecutionEngine> exec_engine_;
    
    std::string config_path_;
    std::unique_ptr<BasketState> basket_state_;

    double entry_z_score_ = 2.0;
    double exit_z_score_ = 0.5;
    
    PositionState current_position_ = PositionState::FLAT;
    
    // Pre-allocated strings to avoid SSO allocations on the hot path
    std::vector<std::string> client_order_ids_;
    
    // Hardcoded daily base allocation for this example (in dollars)
    const double BASE_ALLOCATION = 5000.0; 
};

} // namespace bot