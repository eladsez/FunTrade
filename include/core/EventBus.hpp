#pragma once
#include <core/IEventBus.hpp>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <concurrentqueue.h> // moodycamel lock-free queue

class EventBus : public IEventBus {
public:
    EventBus();
    ~EventBus() override;

    void subscribeToTick(const std::string& symbol, TickCallback callback) override;
    
    // Pushes tick to the lock-free queue immediately (non-blocking)
    void publishTick(const MarketTick& tick) override;

    // Start/stop the background dispatcher thread
    void start();
    void stop();

private:
    void processEvents();

    std::vector<std::pair<std::string, TickCallback>> subscribers;
    std::mutex subMtx; // Mutex only for registering subscribers, not for publishing

    moodycamel::ConcurrentQueue<MarketTick> queue;
    
    std::atomic<bool> isRunning{false};
    std::thread dispatcherThread;
};
