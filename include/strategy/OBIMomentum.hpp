#pragma once
#include <strategy/IStrategy.hpp>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>
#include <memory>
#include <Eigen/Dense>

namespace bot {

class OBIMomentum : public IStrategy {
public:
    OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, int windowSize = 5);
    void start() override;
    void onTick(const MarketTick& tick) override;
    void stop() override;

private:
    std::shared_ptr<IEventBus> eventBus;
    std::shared_ptr<IExecutionEngine> executionEngine;

    // OBI specific variables
    int windowSize_;
    int currentIndex_;
    bool isWindowFull_;
    Eigen::VectorXd obiWindow_;
};

} // namespace bot
