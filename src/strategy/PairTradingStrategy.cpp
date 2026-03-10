#include <strategy/PairTradingStrategy.hpp>
#include <core/Logger.hpp>
#include <numeric>
#include <cmath>
#include <chrono>

namespace bot {

PairTradingStrategy::PairTradingStrategy(
    std::shared_ptr<IEventBus> bus, 
    std::shared_ptr<IExecutionEngine> execEngine, 
    std::shared_ptr<PortfolioManager> portfolioManager,
    std::shared_ptr<IDataFeed> dataFeed,
    const std::string& symbolA, 
    const std::string& symbolB,
    int windowSize,
    double zScoreThreshold)
    : eventBus_(bus), executionEngine_(execEngine), 
      portfolioManager_(portfolioManager), dataFeed_(dataFeed),
      symbolA_(symbolA), symbolB_(symbolB), 
      windowSize_(windowSize), zScoreThreshold_(zScoreThreshold),
      lastPriceA_(0.0), lastPriceB_(0.0), currentPositionState_(0) 
{
}

void PairTradingStrategy::start() {
    LOG_INFO("[PairTrading] Starting strategy for pair {} / {}", symbolA_, symbolB_);

    // 1. Warm up the rolling window using historical data
    warmUpState();

    // 2. Register with EventBus for both symbols
    eventBus_->subscribeToMarketEvents(symbolA_, [this](const MarketEvent& e) { onMarketEvent(e); });
    eventBus_->subscribeToMarketEvents(symbolB_, [this](const MarketEvent& e) { onMarketEvent(e); });

    // 3. Subscribe to real-time data feed
    dataFeed_->subscribeToSymbol(symbolA_);
    dataFeed_->subscribeToSymbol(symbolB_);
}

void PairTradingStrategy::warmUpState() {
    LOG_INFO("[PairTrading] Warming up state using historical data...");
    
    // Define a time range (e.g., last 3 days of 1Min bars)
    // In a real scenario, you should calculate these ISO-8601 strings dynamically based on current time
    std::string start = "2026-03-05T00:00:00Z"; 
    std::string end = "2026-03-08T00:00:00Z";

    auto historyA = dataFeed_->fetchHistoricalBars(symbolA_, start, end, "1Min");
    auto historyB = dataFeed_->fetchHistoricalBars(symbolB_, start, end, "1Min");

    // Simple approach: Match the last N bars. 
    // A robust approach would align them exactly by timestamp_ms.
    size_t barsToProcess = std::min({(size_t)windowSize_, historyA.size(), historyB.size()});
    
    if (barsToProcess == 0) {
        LOG_WARN("[PairTrading] Not enough historical data to warm up.");
        return;
    }

    size_t startIdxA = historyA.size() - barsToProcess;
    size_t startIdxB = historyB.size() - barsToProcess;

    for (size_t i = 0; i < barsToProcess; ++i) {
        double priceA = historyA[startIdxA + i].close;
        double priceB = historyB[startIdxB + i].close;
        double spread = priceA - priceB;
        
        spreadHistory_.push_back(spread);
    }

    LOG_INFO("[PairTrading] Warm up complete. Window size: {}", spreadHistory_.size());
}

void PairTradingStrategy::onMarketEvent(const MarketEvent& event) {
    // Process the incoming tick to update our latest cached prices
    std::visit([this](auto&& tick) {
        using T = std::decay_t<decltype(tick)>;
        
        if constexpr (std::is_same_v<T, TradeTick>) {
            if (tick.symbol == symbolA_) {
                lastPriceA_ = tick.price;
            } else if (tick.symbol == symbolB_) {
                lastPriceB_ = tick.price;
            }
        } 
        else if constexpr (std::is_same_v<T, QuoteTick>) {
            // Calculate mid-price for quotes
            double midPrice = (tick.bid_price + tick.ask_price) / 2.0;
            if (tick.symbol == symbolA_) {
                lastPriceA_ = midPrice;
            } else if (tick.symbol == symbolB_) {
                lastPriceB_ = midPrice;
            }
        }
    }, event);

    // Only evaluate if we have valid real-time prices for both assets
    if (lastPriceA_ > 0.0 && lastPriceB_ > 0.0) {
        evaluateAndTrade();
    }
}

void PairTradingStrategy::evaluateAndTrade() {
    // 1. Calculate current spread
    double currentSpread = lastPriceA_ - lastPriceB_;

    // 2. Update rolling window (fixing the unsigned comparison warning)
    spreadHistory_.push_back(currentSpread);
    if (spreadHistory_.size() > static_cast<size_t>(windowSize_)) {
        spreadHistory_.pop_front();
    }

    if (spreadHistory_.size() < static_cast<size_t>(windowSize_)) {
        return; 
    }

    // 3. Calculate Mean and Standard Deviation
    double sum = std::accumulate(spreadHistory_.begin(), spreadHistory_.end(), 0.0);
    double mean = sum / spreadHistory_.size();

    double sq_sum = std::inner_product(spreadHistory_.begin(), spreadHistory_.end(), spreadHistory_.begin(), 0.0);
    double variance = (sq_sum / spreadHistory_.size()) - (mean * mean);
    double stddev = std::sqrt(variance > 0.0 ? variance : 0.0001); // Prevent division by zero

    // 4. Calculate Z-Score
    double zScore = (currentSpread - mean) / stddev;
    double qty = 10.0; 

    // Helper lambda to cleanly create OrderRequests matching your DomainModels.hpp
    auto sendOrder = [&](const std::string& sym, Side side, double q) {
        OrderRequest req;
        // req.client_order_id is left empty, letting engine/exchange assign it
        req.symbol = sym;
        req.side = side;
        req.type = OrderType::MARKET;
        req.tif = TimeInForce::GTC;
        req.quantity = q;
        executionEngine_->placeOrder(req);
    };

    // 5. Execution Logic
    if (zScore > zScoreThreshold_ && currentPositionState_ != -1) {
        LOG_INFO("[PairTrading] Z-Score {:.2f} > Threshold. Short {}, Long {}", zScore, symbolA_, symbolB_);
        
        if (currentPositionState_ == 1) { /* Send closing orders */ }

        // Execute legs using the helper and correct Side enums
        sendOrder(symbolA_, Side::SELL, qty);
        sendOrder(symbolB_, Side::BUY, qty);
        
        currentPositionState_ = -1;

    } else if (zScore < -zScoreThreshold_ && currentPositionState_ != 1) {
        LOG_INFO("[PairTrading] Z-Score {:.2f} < -Threshold. Long {}, Short {}", zScore, symbolA_, symbolB_);
        
        if (currentPositionState_ == -1) { /* Send closing orders */ }

        sendOrder(symbolA_, Side::BUY, qty);
        sendOrder(symbolB_, Side::SELL, qty);
        
        currentPositionState_ = 1;

    } else if (std::abs(zScore) < 0.5 && currentPositionState_ != 0) {
        LOG_INFO("[PairTrading] Z-Score {:.2f} reverted to mean. Closing positions.", zScore);
        
        if (currentPositionState_ == 1) {
            sendOrder(symbolA_, Side::SELL, qty); 
            sendOrder(symbolB_, Side::BUY, qty);
        } else if (currentPositionState_ == -1) {
            sendOrder(symbolA_, Side::BUY, qty); 
            sendOrder(symbolB_, Side::SELL, qty);
        }
        
        currentPositionState_ = 0;
    }
}

void PairTradingStrategy::stop() {
    LOG_INFO("[PairTrading] Stopping strategy...");
    // Ideally, flatten portfolio and unsubscribe from EventBus
}

} // namespace bot