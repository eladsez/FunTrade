#pragma once
#include <functional>
#include <core/DomainModels.hpp>

namespace bot {

// Callback type for receiving market events.
using MarketEventCallback = std::function<void(const MarketEvent&)>;


// Callback type for receiving execution reports.
using ExecutionCallback = std::function<void(const ExecutionReport&)>;

// Interface for the central message bus. 
// Decouples the data producers (Data Feed) from the consumers (Strategies).
class IEventBus {
public:
    virtual ~IEventBus() = default;

    // returns a unique subscription ID
    virtual uint64_t subscribeToMarketEvents(const std::string& symbol, MarketEventCallback callback) = 0;
    
    // Unsubscribes using the ID
    virtual void unsubscribeFromMarketEvents(const std::string& symbol, uint64_t subId) = 0;

    // Publishes a market events to all registered subscribers.
    virtual void publishMarketEvent(const MarketEvent& event) = 0;

    // Registers a callback to be triggered when an execution report arrives. Returns a unique Sub ID.
    virtual uint64_t subscribeToExecution(ExecutionCallback callback) = 0;

    // Unsubscribes a specific execution report listener using its ID.
    virtual void unsubscribeFromExecution(uint64_t subId) = 0;

    // Publishes an execution report to all registered subscribers.
    virtual void publishExecution(const ExecutionReport& report) = 0;

    // Synchronous call to fetch historical bars from the underlying data feed
    virtual std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string& start, 
        const std::string& end, 
        const std::string& timeframe) = 0;
};

}