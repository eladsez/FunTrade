#pragma once

#include <memory>
#include <vector>
#include <string>

#include <core/IEventBus.hpp>
#include <execution/OrderManager.hpp>
#include <risk/PortfolioManager.hpp>
#include <strategy/IStrategy.hpp>

namespace bot {

class StrategyOrchestrator {
public:
    StrategyOrchestrator(
        std::shared_ptr<IEventBus> eventBus,
        std::shared_ptr<OrderManager> orderMngr,
        std::shared_ptr<PortfolioManager> portfolioMngr,
        const std::string& configDirectory,
        double percentAllocationPerStrategy
    );

    ~StrategyOrchestrator();

    // Iterates through the config directory and initializes strategies
    void loadStrategies();
    
    // Starts all loaded strategies
    void startAll();
    
    // Stops all active strategies
    void stopAll();

    // Returns the number of loaded strategies
    size_t getStrategyCount() const;

private:
    std::shared_ptr<IEventBus> event_bus_;
    std::shared_ptr<OrderManager> order_mngr_;
    std::shared_ptr<PortfolioManager> portfolio_mngr_;
    std::string config_directory_;
    double percent_allocation_per_strategy_;

    std::vector<std::shared_ptr<IStrategy>> strategies_;
};

} // namespace bot