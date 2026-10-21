#pragma once
#include <core/DomainModels.hpp>

namespace bot {

// Base interface for all trading algorithms.
// Complies with the Open/Closed principle: add new strategies without modifying the core.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Initializes the strategy state and registers callbacks with the Event Bus.
    virtual void start() = 0;

    // The core logic loop, triggered upon receiving a new market event.
    virtual void onMarketEvent(const MarketEvent& event) = 0;
    
    // Halts the strategy and cleans up.
    virtual void stop() = 0;

    // Stops for a restart that takes over (a deploy): like stop(), but working orders stay
    // at the broker for the next process to reattach.
    virtual void handOver() { stop(); }
};

}