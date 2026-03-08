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

void EventBus::processEvents() {
    Event event;
    while (isRunning) {
        // Attempt to dequeue
        if (queue.try_dequeue(event)) {
            std::lock_guard<std::mutex> lock(subMtx);
            
            std::visit([this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, MarketTick>) {
                    for (const auto& sub : tickSubscribers) {
                        if (sub.first == arg.symbol) {
                            sub.second(arg);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ExecutionReport>) {
                    for (const auto& sub : executionSubscribers) {
                        sub(arg);
                    }
                }
            }, event);

        } else {
            // Spin-wait or yield to prevent maxing out CPU if queue is empty
            std::this_thread::yield(); 
        }
    }
    
    // Drain remaining events on shutdown
    while (queue.try_dequeue(event)) {
        std::lock_guard<std::mutex> lock(subMtx);
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, MarketTick>) {
                for (const auto& sub : tickSubscribers) {
                    if (sub.first == arg.symbol) {
                        sub.second(arg);
                    }
                }
            } else if constexpr (std::is_same_v<T, ExecutionReport>) {
                for (const auto& sub : executionSubscribers) {
                    sub(arg);
                }
            }
        }, event);
    }
}
