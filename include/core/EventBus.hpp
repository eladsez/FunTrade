#pragma once
#include <core/IEventBus.hpp>
#include <data/IDataFeed.hpp>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <concurrentqueue.h> // moodycamel lock-free queue
#include <variant>
#include <unordered_map>
#include <memory>

namespace bot {

class EventBus : public IEventBus {
public:
    explicit EventBus(std::shared_ptr<IDataFeed> dataFeed);
    ~EventBus() override;

    uint64_t subscribeToMarketEvents(const std::string& symbol, MarketEventCallback callback) override;
    void unsubscribeFromMarketEvents(const std::string& symbol, uint64_t subId) override;
    void publishMarketEvent(const MarketEvent& event);

    uint64_t subscribeToExecution(ExecutionCallback callback) override;
    void unsubscribeFromExecution(uint64_t subId) override;
    void publishExecution(const ExecutionReport& report) override;

    std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string& start, 
        const std::string& end, 
        const std::string& timeframe) override;

    // Start/stop the background dispatcher thread
    void start();
    void stop();

private:
    using Event = std::variant<MarketEvent, ExecutionReport>;
    
    void dispatchEvent(const Event& event);
    void processEvents();

    std::shared_ptr<IDataFeed> dataFeed_;

    // Outer map: Symbol -> Inner map: (SubID -> Callback)
    std::unordered_map<std::string, std::unordered_map<uint64_t, MarketEventCallback>> marketEventSubscribers;
    std::unordered_map<uint64_t, ExecutionCallback> executionSubscribers;

    // Atomic counters for generating unique subscription IDs
    std::atomic<uint64_t> nextMarketSubId{1};
    std::atomic<uint64_t> nextExecSubId{1};

    std::mutex subMtx; // Mutex only for registering subscribers, not for publishing

    moodycamel::ConcurrentQueue<Event> queue;
    
    std::atomic<bool> isRunning{false};
    std::thread dispatcherThread;
};

}