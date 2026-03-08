#include <strategy/OBIMomentum.hpp>
#include <core/Logger.hpp>
#include <cmath>

namespace bot {

OBIMomentum::OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, const std::string& symbol, int windowSize, double stdDevMultiplier, double riskPercentage, int emaWindowSize)
    : eventBus(bus), executionEngine(execEngine), symbol_(symbol), windowSize_(windowSize), currentIndex_(0), isWindowFull_(false), stdDevMultiplier_(stdDevMultiplier), riskPercentage_(riskPercentage), emaWindowSize_(emaWindowSize), emaPrice_(0.0), isEmaInitialized_(false), currentPosition_(PositionState::FLAT) {
    // Initialize Eigen vector for the rolling window
    obiWindow_ = Eigen::VectorXd::Zero(windowSize_);
}

void OBIMomentum::start() {
    LOG_INFO("[OBIMomentum] Strategy started for {} with window size {}. StdDev Multiplier: {}, Risk: {}%, EMA Window: {}. Initial state: FLAT.", symbol_, windowSize_, stdDevMultiplier_, riskPercentage_, emaWindowSize_);
    eventBus->subscribeToTick(symbol_, [this](const MarketTick& tick) {
        this->onTick(tick);
    });
}

void OBIMomentum::onTick(const MarketTick& tick) {
    // If we don't have level 2 data, we can't calculate OBI
    if (tick.bid_size == 0.0 && tick.ask_size == 0.0) {
        return; 
    }

    double midPrice = (tick.bid_price + tick.ask_price) / 2.0;

    // Update EMA Price
    if (!isEmaInitialized_) {
        emaPrice_ = midPrice;
        isEmaInitialized_ = true;
    } else {
        double alpha = 2.0 / (emaWindowSize_ + 1.0);
        emaPrice_ = (alpha * midPrice) + (1.0 - alpha) * emaPrice_;
    }

    // 1. Calculate the raw Order Book Imbalance (OBI)
    double totalSize = tick.bid_size + tick.ask_size;
    double obi = (tick.bid_size - tick.ask_size) / totalSize;

    // 2. Update the rolling window (Eigen Vector)
    obiWindow_(currentIndex_) = obi;
    currentIndex_++;

    if (currentIndex_ >= windowSize_) {
        isWindowFull_ = true;
        currentIndex_ = 0; // Wrap around (circular buffer)
    }

    // 3. Evaluate signal if window is full
    if (isWindowFull_) {
        double smoothedObi = obiWindow_.mean();

        // Calculate standard deviation of OBI
        double variance = (obiWindow_.array() - smoothedObi).square().sum() / (windowSize_ - 1);
        double stdDev = std::sqrt(variance);

        double dynamicBuyThreshold = smoothedObi + (stdDevMultiplier_ * stdDev);
        double dynamicSellThreshold = smoothedObi - (stdDevMultiplier_ * stdDev);

        bool trendIsUp = midPrice > emaPrice_;
        bool trendIsDown = midPrice < emaPrice_;

        // Determine Trade Quantity based on Risk Percentage
        double tradeQty = 0.001; // Safe Default
        auto portfolioValueOpt = executionEngine->getPortfolioValue();
        if (portfolioValueOpt && *portfolioValueOpt > 0) {
            double riskValue = *portfolioValueOpt * (riskPercentage_ / 100.0);
            tradeQty = riskValue / tick.ask_price;
        }

        // 4. Trigger Orders based on dynamic thresholds, EMA confluence AND current position
        if (obi > dynamicBuyThreshold && trendIsUp && currentPosition_ != PositionState::LONG) {
            LOG_INFO(">>> [OBIMomentum] STRONG BUY SIGNAL! Raw OBI ({:.2f}) > Buy Threshold ({:.2f}) & MidPrice ({:.2f}) > EMA ({:.2f}). Entering LONG.", obi, dynamicBuyThreshold, midPrice, emaPrice_);
            
            OrderRequest req{"", symbol_, Side::BUY, OrderType::LIMIT, tradeQty, tick.ask_price}; 
            executionEngine->placeOrder(req);
            
            currentPosition_ = PositionState::LONG;
            isWindowFull_ = false;
            obiWindow_.setZero();
        } 
        else if (obi < dynamicSellThreshold && trendIsDown && currentPosition_ == PositionState::LONG) {
            LOG_INFO("<<< [OBIMomentum] STRONG SELL SIGNAL! Raw OBI ({:.2f}) < Sell Threshold ({:.2f}) & MidPrice ({:.2f}) < EMA ({:.2f}). Exiting LONG to FLAT.", obi, dynamicSellThreshold, midPrice, emaPrice_);
            
            auto positions = executionEngine->getOpenPositions();
            double exactQtyToSell = 0.0;
            if (positions.is_array()) {
                for (const auto& pos : positions) {
                    if (pos.value("symbol", "").find("BTC") != std::string::npos) {
                        exactQtyToSell = std::stod(pos.value("qty_available", "0"));
                        break;
                    }
                }
            }

            if (exactQtyToSell > 0) {
                OrderRequest req{"", symbol_, Side::SELL, OrderType::LIMIT, exactQtyToSell, tick.bid_price};
                executionEngine->placeOrder(req);
            } else {
                LOG_WARN("[OBIMomentum] Strategy tried to sell, but no position was found in Alpaca!");
            }
            
            currentPosition_ = PositionState::FLAT;
            isWindowFull_ = false;
            obiWindow_.setZero();
        }
    }
}

void OBIMomentum::stop() {
    LOG_INFO("[OBIMomentum] Strategy stopped.");
}

} // namespace bot
