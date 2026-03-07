#include <strategy/OBIMomentum.hpp>
#include <iostream>

namespace bot {

OBIMomentum::OBIMomentum(std::shared_ptr<IEventBus> bus, std::shared_ptr<IExecutionEngine> execEngine, int windowSize)
    : eventBus(bus), executionEngine(execEngine), windowSize_(windowSize), currentIndex_(0), isWindowFull_(false) {
    // Initialize Eigen vector for the rolling window
    obiWindow_ = Eigen::VectorXd::Zero(windowSize_);
}

void OBIMomentum::start() {
    std::cout << "[OBIMomentum] Strategy started with window size " << windowSize_ << ".\n";
    eventBus->subscribeToTick("BTC/USD", [this](const MarketTick& tick) {
        this->onTick(tick);
    });
}

void OBIMomentum::onTick(const MarketTick& tick) {
    // If we don't have level 2 data, we can't calculate OBI
    if (tick.bid_size == 0.0 && tick.ask_size == 0.0) {
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
            OrderRequest req{"BTC/USD", Side::BUY, OrderType::MARKET, 1.0, 0.0};
            executionEngine->placeOrder(req);
            
            // To prevent spamming, we might want to clear the window or implement a cooldown,
            // but for this PoC, we will just reset the window state.
            isWindowFull_ = false;
            obiWindow_.setZero();
        } 
        else if (smoothedObi < -0.4) {
            std::cout << "<<< [OBIMomentum] STRONG SELL SIGNAL! Smoothed OBI < -0.4\n";
            OrderRequest req{"BTC/USD", Side::SELL, OrderType::MARKET, 1.0, 0.0};
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
