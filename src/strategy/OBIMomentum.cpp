#include <strategy/OBIMomentum.hpp>
#include <core/Logger.hpp>

namespace bot {

OBIMomentum::OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, const std::string& symbol, int windowSize, double buyThreshold, double sellThreshold)
    : eventBus(bus), executionEngine(execEngine), symbol_(symbol), windowSize_(windowSize), currentIndex_(0), isWindowFull_(false), buyThreshold_(buyThreshold), sellThreshold_(sellThreshold), currentPosition_(PositionState::FLAT) {
    // Initialize Eigen vector for the rolling window
    obiWindow_ = Eigen::VectorXd::Zero(windowSize_);
}

void OBIMomentum::start() {
    LOG_INFO("[OBIMomentum] Strategy started for {} with window size {}. Thresholds: BUY > {}, SELL < {}. Initial state: FLAT.", symbol_, windowSize_, buyThreshold_, sellThreshold_);
    eventBus->subscribeToTick(symbol_, [this](const MarketTick& tick) {
        this->onTick(tick);
    });
}

void OBIMomentum::onTick(const MarketTick& tick) {
    // If we don't have level 2 data, we can't calculate OBI
    if (tick.bid_size == 0.0 && tick.ask_size == 0.0) {
        // Fallback or print for live debug to see prices moving even without L2 OBI
        // LOG_DEBUG("[DEBUG] {} price: {}", tick.symbol, tick.price);
        return; 
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
        // Calculate the Simple Moving Average using Eigen's built-in mean()
        double smoothedObi = obiWindow_.mean();

        // Print less frequently, only print if we are close to a threshold
        if (std::abs(smoothedObi) > 0.3) {
             LOG_INFO("[OBIMomentum] Tick @ {} | Raw OBI: {:.2f} | Smoothed OBI (SMA): {:.2f}", tick.price, obi, smoothedObi);
        }

        // 4. Trigger Orders based on momentum thresholds AND current position
        if (smoothedObi > buyThreshold_ && currentPosition_ != PositionState::LONG) {
            LOG_INFO(">>> [OBIMomentum] STRONG BUY SIGNAL! Smoothed OBI ({:.2f}) > {}. Entering LONG position.", smoothedObi, buyThreshold_);
            
            OrderRequest req{symbol_, Side::BUY, OrderType::MARKET, 0.001, 0.0}; 
            executionEngine->placeOrder(req);
            
            currentPosition_ = PositionState::LONG;
            isWindowFull_ = false;
            obiWindow_.setZero();
        } 
        else if (smoothedObi < sellThreshold_ && currentPosition_ == PositionState::LONG) {
            // Alpaca Crypto does NOT support short selling. We can only sell if we are currently LONG.
            LOG_INFO("<<< [OBIMomentum] STRONG SELL SIGNAL! Smoothed OBI ({:.2f}) < {}. Exiting LONG position to FLAT.", smoothedObi, sellThreshold_);
            
            // In a production bot, we must query the EXACT available balance to sell to avoid fractional fee failures.
            // For this PoC, we will query the execution engine dynamically right here before selling.
            auto positions = executionEngine->getOpenPositions();
            double exactQtyToSell = 0.0;
            if (positions.is_array()) {
                for (const auto& pos : positions) {
                    // Match BTCUSD or BTC/USD
                    if (pos.value("symbol", "").find("BTC") != std::string::npos) {
                        exactQtyToSell = std::stod(pos.value("qty_available", "0"));
                        break;
                    }
                }
            }

            if (exactQtyToSell > 0) {
                OrderRequest req{symbol_, Side::SELL, OrderType::MARKET, exactQtyToSell, 0.0};
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
