#include <core/EventBus.hpp>
#include <core/Logger.hpp>
#include <chrono>

EventBus::EventBus(std::shared_ptr<IDataFeed> dataFeed) : dataFeed_(std::move(dataFeed)) {
    // If a data feed is provided, wire its output directly to our lock-free queue
    if (dataFeed_) {
        dataFeed_->setMarketTickCallback([this](const MarketEvent& tick) {
            this->publishMarketEvent(tick);
        });
    }
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

uint64_t EventBus::subscribeToMarketEvents(const std::string& symbol, MarketEventCallback callback) {
    std::lock_guard<std::mutex> lock(subMtx);
    uint64_t subId = nextMarketSubId++;

    // If this is the very first listener for this symbol, notify the DataFeed
    if (marketEventSubscribers.find(symbol) == marketEventSubscribers.end() || 
        marketEventSubscribers[symbol].empty()) {
        if (dataFeed_) {
            dataFeed_->subscribeToSymbol(symbol);
        }
    }

    marketEventSubscribers[symbol][subId] = std::move(callback);
    LOG_INFO("[EventBus] Subscribed to tick: {} (ID: {})", symbol, subId);
    return subId;
}

void EventBus::unsubscribeFromMarketEvents(const std::string& symbol, uint64_t subId) {
    std::lock_guard<std::mutex> lock(subMtx);
    
    auto it = marketEventSubscribers.find(symbol);
    if (it != marketEventSubscribers.end()) {
        it->second.erase(subId);
        
        // If no one is listening to this symbol anymore, tell the DataFeed to unsubscribe
        if (it->second.empty()) {
            if (dataFeed_) {
                dataFeed_->unsubscribeFromSymbol(symbol);
            }
            marketEventSubscribers.erase(it);
        }
    }
}

void EventBus::publishMarketEvent(const MarketEvent& event) {
    // Lock-free enqueue: Network thread returns immediately
    queue.enqueue(event);
}

uint64_t EventBus::subscribeToExecution(ExecutionCallback callback) {
    std::lock_guard<std::mutex> lock(subMtx);
    uint64_t subId = nextExecSubId++;
    executionSubscribers[subId] = std::move(callback);
    LOG_INFO("[EventBus] Subscribed to execution reports (ID: {})", subId);
    return subId;
}

void EventBus::unsubscribeFromExecution(uint64_t subId) {
    std::lock_guard<std::mutex> lock(subMtx);
    executionSubscribers.erase(subId);
}

void EventBus::publishExecution(const ExecutionReport& report) {
    queue.enqueue(report);
}

void EventBus::dispatchEvent(const Event& event) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, MarketEvent>) {
            // Safely extract the symbol from the variant
            std::string symbol = std::visit([](auto&& e) { return e.symbol; }, arg);
            
            // Snapshot ONLY the specific symbol's inner map to keep the lock ultra-fast
            std::unordered_map<uint64_t, MarketEventCallback> localCallbacks;
            {
                std::lock_guard<std::mutex> lock(subMtx);
                auto it = marketEventSubscribers.find(symbol);
                if (it != marketEventSubscribers.end()) {
                    localCallbacks = it->second;
                }
            }

            // Execute the callbacks safely outside the lock
            for (const auto& [id, callback] : localCallbacks) {
                callback(arg);
            }
        } 
        else if constexpr (std::is_same_v<T, ExecutionReport>) {
            std::unordered_map<uint64_t, ExecutionCallback> localExecSubs;
            {
                std::lock_guard<std::mutex> lock(subMtx);
                localExecSubs = executionSubscribers;
            }

            for (const auto& [id, callback] : localExecSubs) {
                callback(arg);
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

std::vector<HistoricalBar> EventBus::fetchHistoricalBars(
    const std::string& symbol, 
    const std::string& start, 
    const std::string& end, 
    const std::string& timeframe) {
    
    // Safety check: Ensure the data feed is actually wired up
    if (dataFeed_) {
        return dataFeed_->fetchHistoricalBars(symbol, start, end, timeframe);
    }
    
    LOG_ERROR("[EventBus] Cannot fetch historical bars for {}: No DataFeed attached!", symbol);
    return {}; // Return an empty vector if there is no feed
}