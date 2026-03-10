#pragma once

#include <strategy/IStrategy.hpp>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>
#include <risk/PortfolioManager.hpp>
#include <data/IDataFeed.hpp>
#include <memory>
#include <string>
#include <deque>

namespace bot {

class PairTradingStrategy : public IStrategy {
public:
    PairTradingStrategy(std::shared_ptr<IEventBus> bus, 
                        std::shared_ptr<IExecutionEngine> execEngine, 
                        std::shared_ptr<PortfolioManager> portfolioManager,
                        std::shared_ptr<IDataFeed> dataFeed,
                        const std::string& symbolA, 
                        const std::string& symbolB,
                        int windowSize = 100,
                        double zScoreThreshold = 2.0);

    void start() override;
    void onMarketEvent(const MarketEvent& event) override;
    void stop() override;

private:
    void warmUpState();
    void evaluateAndTrade();

    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> executionEngine_;
    std::shared_ptr<PortfolioManager> portfolioManager_;
    std::shared_ptr<IDataFeed> dataFeed_;

    std::string symbolA_;
    std::string symbolB_;

    // Parameters
    int windowSize_;
    double zScoreThreshold_;

    // Asynchronous State Tracking
    double lastPriceA_;
    double lastPriceB_;
    std::deque<double> spreadHistory_;

    // Track current position: 
    //  0 = Flat
    //  1 = Long A, Short B
    // -1 = Short A, Long B
    int currentPositionState_; 
};

} // namespace bot