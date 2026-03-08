#pragma once
#include <functional>
#include <core/DomainModels.hpp>

// Callback type for receiving ticks.
using TickCallback = std::function<void(const MarketTick&)>;

// Callback type for receiving execution reports.
using ExecutionCallback = std::function<void(const ExecutionReport&)>;

// Interface for the central message bus. 
// Decouples the data producers (Data Feed) from the consumers (Strategies).
class IEventBus {
public:
    virtual ~IEventBus() = default;

    // Registers a callback to be triggered when a tick arrives for a symbol.
    virtual void subscribeToTick(const std::string& symbol, TickCallback callback) = 0;

    // Publishes a tick to all registered subscribers.
    virtual void publishTick(const MarketTick& tick) = 0;

    // Registers a callback to be triggered when an execution report arrives.
    virtual void subscribeToExecution(ExecutionCallback callback) = 0;

    // Publishes an execution report to all registered subscribers.
    virtual void publishExecution(const ExecutionReport& report) = 0;
};