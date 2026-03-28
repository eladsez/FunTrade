#include <strategy/StrategyOrchestrator.hpp>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <core/Logger.hpp>
#include <filesystem>

namespace fs = std::filesystem;

namespace bot {

StrategyOrchestrator::StrategyOrchestrator(
    std::shared_ptr<IEventBus> eventBus,
    std::shared_ptr<OrderManager> orderMngr,
    std::shared_ptr<PortfolioManager> portfolioMngr,
    const std::string& configDirectory,
    double percentAllocationPerStrategy)
    : event_bus_(std::move(eventBus)),
      order_mngr_(std::move(orderMngr)),
      portfolio_mngr_(std::move(portfolioMngr)),
      config_directory_(configDirectory),
      percent_allocation_per_strategy_(percentAllocationPerStrategy) {}

void StrategyOrchestrator::loadStrategies() {

    if (!fs::exists(config_directory_) || !fs::is_directory(config_directory_)) {
        LOG_ERROR("[Orchestrator] Config directory does not exist or is invalid: {}", config_directory_);
        return;
    }

    double availableCash = 100000.0; // Safe default for backtesting
    auto cashOpt = portfolio_mngr_->getAvailableCash();
    if (cashOpt) {
        availableCash = *cashOpt;
    }

    double capital_per_strategy = availableCash * percent_allocation_per_strategy_;
    LOG_INFO("[Orchestrator] Total Available Cash: ${}. Allocating {:.2f}% -> ${} per strategy.", 
             availableCash, percent_allocation_per_strategy_ * 100.0, capital_per_strategy);
    
    // Iterate over the directory and find all JSON files
    for (const auto& entry : fs::directory_iterator(config_directory_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            std::string configPath = entry.path().string();
            LOG_INFO("[Orchestrator] Initializing strategy for config: {}", configPath);
    
            auto strategy = std::make_shared<BasketMeanReversionStrategy>(
                event_bus_,
                order_mngr_,
                portfolio_mngr_,
                configPath,
                capital_per_strategy // Pass the calculated absolute dollar amount
            );
            
            strategies_.push_back(std::move(strategy));
        }
    }
    
    LOG_DEBUG("[Orchestrator] Successfully loaded {} strategies.", strategies_.size());
}

void StrategyOrchestrator::startAll() {
    LOG_INFO("[Orchestrator] Starting all {} strategies...", strategies_.size());
    for (auto& strategy : strategies_) {
        strategy->start();
    }
}

void StrategyOrchestrator::stopAll() {
    LOG_INFO("[Orchestrator] Stopping all strategies...");
    for (auto& strategy : strategies_) {
        strategy->stop();
    }
}

size_t StrategyOrchestrator::getStrategyCount() const {
    return strategies_.size();
}

} // namespace bot