#pragma once
#include <functional>
#include <core/DomainModels.hpp>

// Callback type for receiving ticks.
using TickCallback = std::function<void(const MarketTick&)>;

// Interface for the central message bus. 
// Decouples the data producers (Data Feed) from the consumers (Strategies).
class IEventBus {
public:
    virtual ~IEventBus() = default;

    // Registers a callback to be triggered when a tick arrives for a symbol.
    virtual void subscribeToTick(const std::string& symbol, TickCallback callback) = 0;

    // Publishes a tick to all registered subscribers.
    virtual void publishTick(const MarketTick& tick) = 0;
};