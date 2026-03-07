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
    subscribers.push_back({symbol, callback});
    LOG_INFO("[EventBus] Subscribed to {}", symbol);
}

void EventBus::publishTick(const MarketTick& tick) {
    // Lock-free enqueue: Network thread returns immediately
    queue.enqueue(tick);
}

void EventBus::processEvents() {
    MarketTick tick;
    while (isRunning) {
        // Attempt to dequeue
        if (queue.try_dequeue(tick)) {
            // Read-only access to subscribers list for simplicity here.
            // In a highly dynamic system, we'd use a reader-writer lock or a lock-free list for subscribers.
            // But since strategies typically subscribe once at startup, a simple lock or copy is fine.
            std::lock_guard<std::mutex> lock(subMtx);
            for (const auto& sub : subscribers) {
                if (sub.first == tick.symbol) {
                    sub.second(tick);
                }
            }
        } else {
            // Spin-wait or yield to prevent maxing out CPU if queue is empty
            std::this_thread::yield(); 
            // Alternately, could use std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
    
    // Drain remaining events on shutdown
    while (queue.try_dequeue(tick)) {
        std::lock_guard<std::mutex> lock(subMtx);
        for (const auto& sub : subscribers) {
            if (sub.first == tick.symbol) {
                sub.second(tick);
            }
        }
    }
}
