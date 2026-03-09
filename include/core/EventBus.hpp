#pragma once
#include <core/IEventBus.hpp>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <concurrentqueue.h> // moodycamel lock-free queue

#include <variant>

class EventBus : public IEventBus {
public:
    EventBus();
    ~EventBus() override;

    void subscribeToTick(const std::string& symbol, TickCallback callback) override;
    void publishTick(const MarketTick& tick) override;

    void subscribeToExecution(ExecutionCallback callback) override;
    void publishExecution(const ExecutionReport& report) override;

    // Start/stop the background dispatcher thread
    void start();
    void stop();

private:
    using Event = std::variant<MarketTick, ExecutionReport>;
    
    void dispatchEvent(const Event& event);
    void processEvents();


    std::vector<std::pair<std::string, TickCallback>> tickSubscribers;
    std::vector<ExecutionCallback> executionSubscribers;
    std::mutex subMtx; // Mutex only for registering subscribers, not for publishing

    moodycamel::ConcurrentQueue<Event> queue;
    
    std::atomic<bool> isRunning{false};
    std::thread dispatcherThread;
};
