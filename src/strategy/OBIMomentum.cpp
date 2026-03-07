#include <strategy/OBIMomentum.hpp>
#include <iostream>

namespace bot {

OBIMomentum::OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, const std::string& symbol, int windowSize)
    : eventBus(bus), executionEngine(execEngine), symbol_(symbol), windowSize_(windowSize), currentIndex_(0), isWindowFull_(false) {
    // Initialize Eigen vector for the rolling window
    obiWindow_ = Eigen::VectorXd::Zero(windowSize_);
}

void OBIMomentum::start() {
    std::cout << "[OBIMomentum] Strategy started for " << symbol_ << " with window size " << windowSize_ << ".\n";
    eventBus->subscribeToTick(symbol_, [this](const MarketTick& tick) {
        this->onTick(tick);
    });
}

void OBIMomentum::onTick(const MarketTick& tick) {
    // If we don't have level 2 data, we can't calculate OBI
    if (tick.bid_size == 0.0 && tick.ask_size == 0.0) {
        // Fallback or print for live debug to see prices moving even without L2 OBI
        // std::cout << "[DEBUG] " << tick.symbol << " price: " << tick.price << "\n";
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

        std::cout << "[OBIMomentum] Tick @ " << tick.price 
                  << " | Raw OBI: " << obi 
                  << " | Smoothed OBI (SMA): " << smoothedObi << "\n";

        // 4. Trigger Orders based on momentum thresholds
        if (smoothedObi > 0.4) {
            std::cout << ">>> [OBIMomentum] STRONG BUY SIGNAL! Smoothed OBI > 0.4\n";
            OrderRequest req{symbol_, Side::BUY, OrderType::MARKET, 0.001, 0.0}; // Smaller qty for crypto
            executionEngine->placeOrder(req);
            
            isWindowFull_ = false;
            obiWindow_.setZero();
        } 
        else if (smoothedObi < -0.4) {
            std::cout << "<<< [OBIMomentum] STRONG SELL SIGNAL! Smoothed OBI < -0.4\n";
            OrderRequest req{symbol_, Side::SELL, OrderType::MARKET, 0.001, 0.0};
            executionEngine->placeOrder(req);
            
            isWindowFull_ = false;
            obiWindow_.setZero();
        }
    }
}

void OBIMomentum::stop() {
    std::cout << "[OBIMomentum] Strategy stopped.\n";
}

} // namespace bot
