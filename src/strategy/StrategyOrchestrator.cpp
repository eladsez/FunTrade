#include <strategy/StrategyOrchestrator.hpp>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <core/Logger.hpp>
#include <risk/UiBroadcaster.hpp>
#include <filesystem>

namespace fs = std::filesystem;

namespace bot {

StrategyOrchestrator::StrategyOrchestrator(
    std::shared_ptr<IEventBus> eventBus,
    std::shared_ptr<OrderManager> orderMngr,
    std::shared_ptr<PortfolioManager> portfolioMngr,
    const std::string& configDirectory,
    double percentAllocationPerStrategy,
    StrategyOptions options)
    : event_bus_(std::move(eventBus)),
      order_mngr_(std::move(orderMngr)),
      portfolio_mngr_(std::move(portfolioMngr)),
      config_directory_(configDirectory),
      percent_allocation_per_strategy_(percentAllocationPerStrategy),
      options_(std::move(options)) {
    UiBroadcaster::getInstance().startServer(9000);
}

StrategyOrchestrator::~StrategyOrchestrator(){
    UiBroadcaster::getInstance().stopServer();
}

void StrategyOrchestrator::loadStrategies() {

    if (!fs::exists(config_directory_) || !fs::is_directory(config_directory_)) {
        LOG_ERROR("[Orchestrator] Config directory does not exist or is invalid: {}", config_directory_);
        return;
    }

    // Size from account equity (USD net liquidation). Without it we can't size safely.
    auto equityOpt = portfolio_mngr_->getPortfolioValue();
    if (!equityOpt || *equityOpt <= 0.0) {
        LOG_ERROR("[Orchestrator] No account value from the broker; refusing to load strategies.");
        return;
    }
    double availableCash = *equityOpt;

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
                capital_per_strategy, // Pass the calculated absolute dollar amount
                options_
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

void StrategyOrchestrator::stopAll(bool handOver) {
    LOG_INFO("[Orchestrator] Stopping all strategies{}...", handOver ? " (handover: working orders stay)" : "");
    for (auto& strategy : strategies_) {
        if (handOver) strategy->handOver();
        else strategy->stop();
    }
}

size_t StrategyOrchestrator::getStrategyCount() const {
    return strategies_.size();
}

} // namespace bot