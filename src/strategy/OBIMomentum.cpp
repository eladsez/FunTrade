#include <strategy/OBIMomentum.hpp>
#include <core/Logger.hpp>
#include <cmath>
#include <algorithm>

namespace bot {

OBIMomentum::OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, std::shared_ptr<PortfolioManager> portfolioManager, double initialPortfolioValue, const std::string& symbol, int windowSize, double stdDevMultiplier, double riskPercentage, int emaWindowSize)
    : eventBus(bus), executionEngine(execEngine), portfolioManager_(portfolioManager), symbol_(symbol), windowSize_(windowSize), currentIndex_(0), isWindowFull_(false), stdDevMultiplier_(stdDevMultiplier), riskPercentage_(riskPercentage), emaWindowSize_(emaWindowSize), emaPrice_(0.0), isEmaInitialized_(false), portfolioValue_(initialPortfolioValue) {
    // Initialize Eigen vector for the rolling window
    obiWindow_ = Eigen::VectorXd::Zero(windowSize_);
}

void OBIMomentum::start() {
    LOG_INFO("[OBIMomentum] Strategy started for {} with window size {}. StdDev Multiplier: {}, Risk: {}%, EMA Window: {}. Initial PV: ${}", symbol_, windowSize_, stdDevMultiplier_, riskPercentage_, emaWindowSize_, portfolioValue_);

    eventBus->subscribeToTick(symbol_, [this](const MarketTick& tick) {
        this->onTick(tick);
    });
}

void OBIMomentum::onTick(const MarketTick& tick) {
    LOG_DEBUG("[OBIMomentum] MarketTick for {} Price: {:.2f}, Vol: {:.4f}, TS: {}, Bid: {:.4f}@{:.2f}, Ask: {:.4f}@{:.2f}", 
          tick.symbol, 
          tick.price, 
          tick.volume, 
          tick.timestamp_ms,
          tick.bid_size, 
          tick.bid_price, 
          tick.ask_size, 
          tick.ask_price);
    // If we don't have level 2 data, we can't calculate OBI
    if (tick.type != TickType::QUOTE && tick.type != TickType::ORDERBOOK) {
        return; 
    }

    // Get current local system time in milliseconds
    auto now = std::chrono::system_clock::now();
    uint64_t localTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    // Guard against stale data (e.g., discard ticks older than 500ms)
    if (tick.timestamp_ms > 0 && (localTimeMs - tick.timestamp_ms > 500)) {
        LOG_DEBUG("[OBIMomentum] Discarding stale tick for {} ({}ms old)", tick.symbol, (localTimeMs - tick.timestamp_ms));
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

    // 1. Calculate the raw Order Book Imbalance (OBI) for the CURRENT tick
    double totalSize = tick.bid_size + tick.ask_size;
    double currentObi = (tick.bid_size - tick.ask_size) / totalSize;

    // 2. Evaluate signal against the historical window BEFORE adding the new OBI
    if (isWindowFull_) {
        double smoothedObi = obiWindow_.mean();

        // Calculate standard deviation of historical OBI
        double variance = (obiWindow_.array() - smoothedObi).square().sum() / (windowSize_ - 1);
        double stdDev = std::sqrt(variance);

        // Prevent 0 stdDev dividing/thresholding issues
        if (stdDev < 1e-6) stdDev = 1e-6;

        double dynamicBuyThreshold = smoothedObi + (stdDevMultiplier_ * stdDev);
        double dynamicSellThreshold = smoothedObi - (stdDevMultiplier_ * stdDev);

        bool trendIsUp = midPrice > emaPrice_;
        bool trendIsDown = midPrice < emaPrice_;

        // Use PortfolioManager to get the current ACTUAL position quantity
        std::string cleanSymbol = symbol_;
        cleanSymbol.erase(std::remove(cleanSymbol.begin(), cleanSymbol.end(), '/'), cleanSymbol.end());
        double netQty = portfolioManager_->getNetPosition(cleanSymbol);
        bool isLong = netQty > 0.0001;

        // Portfolio value & risk sizing
        double currentPortfolioValue = portfolioValue_ + portfolioManager_->getTotalRealizedPnL() + portfolioManager_->getTotalUnrealizedPnL();
        double riskValue = currentPortfolioValue * (riskPercentage_ / 100.0);
        double tradeQty = riskValue / tick.ask_price;
        if (tradeQty < 0.0001) tradeQty = 0.0001; // Minimum trade size guard

        // 3. Trigger Orders based on historical thresholds
        if (currentObi > dynamicBuyThreshold && trendIsUp && !isLong) {
            LOG_INFO(">>> [OBIMomentum] STRONG BUY SIGNAL! Raw OBI ({:.2f}) > Buy Threshold ({:.2f}) & MidPrice ({:.2f}) > EMA ({:.2f}). Entering LONG.", currentObi, dynamicBuyThreshold, midPrice, emaPrice_);
            
            OrderRequest req{"", symbol_, Side::BUY, OrderType::LIMIT, TimeInForce::GTC, tradeQty, tick.ask_price}; 
            executionEngine->placeOrder(req);
            
            // FIX: Do NOT zero out the window. We just reset the index and fullness 
            // to act as a brief cooldown while we wait for the asynchronous fill.
            isWindowFull_ = false;
            currentIndex_ = 0;
        } 
        else if (currentObi < dynamicSellThreshold && trendIsDown && isLong) {
            LOG_INFO("<<< [OBIMomentum] STRONG SELL SIGNAL! Raw OBI ({:.2f}) < Sell Threshold ({:.2f}) & MidPrice ({:.2f}) < EMA ({:.2f}). Exiting LONG to FLAT.", currentObi, dynamicSellThreshold, midPrice, emaPrice_);
            
            OrderRequest req{"", symbol_, Side::SELL, OrderType::LIMIT, TimeInForce::GTC, netQty, tick.bid_price};
            executionEngine->placeOrder(req);
            
            // FIX: Same here. Act as a cooldown, keep the historical data alive.
            isWindowFull_ = false;
            currentIndex_ = 0;
        }
    }

    // 4. NOW update the rolling window with the new OBI
    obiWindow_(currentIndex_) = currentObi;
    currentIndex_++;

    if (currentIndex_ >= windowSize_) {
        isWindowFull_ = true;
        currentIndex_ = 0; // Wrap around (circular buffer)
    }
}

void OBIMomentum::stop() {
    LOG_INFO("[OBIMomentum] Strategy stopped.");
}

} // namespace bot