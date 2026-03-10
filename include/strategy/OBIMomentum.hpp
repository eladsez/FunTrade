#pragma once
#include <strategy/IStrategy.hpp>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>
#include <risk/PortfolioManager.hpp>
#include <memory>
#include <Eigen/Dense>

namespace bot {

class OBIMomentum : public IStrategy {
public:
    OBIMomentum(std::shared_ptr<IEventBus> bus, 
                std::shared_ptr<IExecutionEngine> execEngine, 
                std::shared_ptr<PortfolioManager> portfolioManager,
                double initialPortfolioValue = 100000.0,
                const std::string& symbol = "BTC/USD", 
                int windowSize = 5,
                double stdDevMultiplier = 2.0,
                double riskPercentage = 1.0,
                int emaWindowSize = 10);
    void start() override;
    virtual void onMarketEvent(const MarketEvent& event);
    void stop() override;

private:
    std::shared_ptr<IEventBus> eventBus;
    std::shared_ptr<IExecutionEngine> executionEngine;
    std::shared_ptr<PortfolioManager> portfolioManager_;
    std::string symbol_;

    // OBI specific variables
    int windowSize_;
    int currentIndex_;
    bool isWindowFull_;
    Eigen::VectorXd obiWindow_;
    
    double stdDevMultiplier_;
    double riskPercentage_;
    
    // EMA for Signal Confluence
    int emaWindowSize_;
    double emaPrice_;
    bool isEmaInitialized_;

    // Portfolio state
    double portfolioValue_;
};

} // namespace bot
