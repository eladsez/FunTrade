#pragma once
#include <functional>
#include <core/DomainModels.hpp>

// Callback type for receiving market events.
using MarketEventCallback = std::function<void(const MarketEvent&)>;


// Callback type for receiving execution reports.
using ExecutionCallback = std::function<void(const ExecutionReport&)>;

// Interface for the central message bus. 
// Decouples the data producers (Data Feed) from the consumers (Strategies).
class IEventBus {
public:
    virtual ~IEventBus() = default;

    // Registers a callback to be triggered when a event arrives for a symbol.
    virtual void subscribeToMarketEvents(const std::string& symbol, MarketEventCallback callback) = 0;

    // Publishes a market events to all registered subscribers.
    virtual void publishMarketEvent(const MarketEvent& event) = 0;

    // Registers a callback to be triggered when an execution report arrives.
    virtual void subscribeToExecution(ExecutionCallback callback) = 0;

    // Publishes an execution report to all registered subscribers.
    virtual void publishExecution(const ExecutionReport& report) = 0;
};