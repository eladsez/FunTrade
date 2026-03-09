#include <core/EventBus.hpp>
#include <core/Logger.hpp>
#include <chrono>

EventBus::EventBus() {
    start();
}

EventBus::~EventBus() {
    stop();
}

void EventBus::start() {
    if (isRunning.exchange(true)) return;
    
    dispatcherThread = std::thread(&EventBus::processEvents, this);
    LOG_INFO("[EventBus] Background dispatcher thread started.");
}

void EventBus::stop() {
    if (!isRunning.exchange(false)) return;

    if (dispatcherThread.joinable()) {
        dispatcherThread.join();
    }
    LOG_INFO("[EventBus] Background dispatcher thread stopped.");
}

void EventBus::subscribeToTick(const std::string& symbol, TickCallback callback) {
    std::lock_guard<std::mutex> lock(subMtx);
    tickSubscribers.push_back({symbol, callback});
    LOG_INFO("[EventBus] Subscribed to tick: {}", symbol);
}

void EventBus::publishTick(const MarketTick& tick) {
    // Lock-free enqueue: Network thread returns immediately
    queue.enqueue(tick);
}

void EventBus::subscribeToExecution(ExecutionCallback callback) {
    std::lock_guard<std::mutex> lock(subMtx);
    executionSubscribers.push_back(callback);
    LOG_INFO("[EventBus] Subscribed to execution reports");
}

void EventBus::publishExecution(const ExecutionReport& report) {
    queue.enqueue(report);
}

void EventBus::dispatchEvent(const Event& event) {
    // 1. Snapshot the subscribers quickly under the lock to prevent deadlocks
    std::vector<std::pair<std::string, TickCallback>> localTickSubs;
    std::vector<ExecutionCallback> localExecSubs;
    {
        std::lock_guard<std::mutex> lock(subMtx);
        localTickSubs = tickSubscribers;
        localExecSubs = executionSubscribers;
    } // Lock is completely released here!

    // 2. Safely execute the callbacks using the local copies
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, MarketTick>) {
            for (const auto& sub : localTickSubs) {
                if (sub.first == arg.symbol) {
                    sub.second(arg);
                }
            }
        } 
        else if constexpr (std::is_same_v<T, ExecutionReport>) {
            for (const auto& sub : localExecSubs) {
                sub(arg);
            }
        }
    }, event);
}

void EventBus::processEvents() {
    Event event;
    
    // Main live processing loop
    while (isRunning) {
        if (queue.try_dequeue(event)) {
            dispatchEvent(event);
        } else {
            // Spin-wait or yield to prevent maxing out CPU
            std::this_thread::yield(); 
        }
    }
    
    // Drain remaining events cleanly on shutdown
    while (queue.try_dequeue(event)) {
        dispatchEvent(event);
    }
}