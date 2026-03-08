#pragma once
#include <strategy/IStrategy.hpp>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>
#include <memory>
#include <Eigen/Dense>

namespace bot {

enum class PositionState {
    FLAT,
    LONG,
    SHORT
};

class OBIMomentum : public IStrategy {
public:
    OBIMomentum(std::shared_ptr<IEventBus> bus, 
                std::shared_ptr<IExecutionEngine> execEngine, 
                const std::string& symbol = "BTC/USD", 
                int windowSize = 5,
                double stdDevMultiplier = 2.0,
                double riskPercentage = 1.0,
                int emaWindowSize = 10);
    void start() override;
    void onTick(const MarketTick& tick) override;
    void stop() override;

private:
    std::shared_ptr<IEventBus> eventBus;
    std::shared_ptr<IExecutionEngine> executionEngine;
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

    // Position tracking to prevent order spamming
    PositionState currentPosition_;
};

} // namespace bot
