#include <core/EventBus.hpp>
#include <core/Logger.hpp>
#include <chrono>
#include <future>

namespace bot {

EventBus::EventBus(std::shared_ptr<IDataFeed> dataFeed, std::shared_ptr<IExecutionEngine> executionEngine,
                   DispatchMode mode)
                    : dataFeed_(std::move(dataFeed)), executionEngine_(std::move(executionEngine)), mode_(mode) {
    if (dataFeed_) {
        dataFeed_->setMarketTickCallback([this](const MarketEvent& tick) {
            this->publishMarketEvent(tick);
        });
    }
    if (executionEngine_){
        executionEngine_->setExecutionReportCallback([this] (const ExecutionReport& rep){
            this->publishExecution(rep);
        });
    }
    start();
}

EventBus::~EventBus() {
    stop();
}

void EventBus::start() {
    if (mode_ == DispatchMode::Synchronous) return;
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
    if (symbol != "*" && (marketEventSubscribers.find(symbol) == marketEventSubscribers.end() || 
        marketEventSubscribers[symbol].empty())) {
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

void EventBus::enqueue(Event event) {
    if (mode_ == DispatchMode::Synchronous) {
        // Recursive: a callback may publish (e.g. the mock engine filling an order), which is
        // then delivered inline, exactly like a synchronous exchange would.
        std::lock_guard<std::recursive_mutex> lock(syncDispatchMtx_);
        dispatchEvent(event);
        return;
    }
    // Lock-free enqueue: Network thread returns immediately
    queue.enqueue(std::move(event));
}

void EventBus::publishMarketEvent(const MarketEvent& event) {
    enqueue(event);
}

void EventBus::post(std::function<void()> task) {
    enqueue(Task(std::move(task)));
}

void EventBus::flush() {
    if (mode_ == DispatchMode::Synchronous || !isRunning) return;
    if (std::this_thread::get_id() == dispatcherThread.get_id()) return;  // would deadlock
    std::promise<void> done;
    auto fut = done.get_future();
    post([&done] { done.set_value(); });
    fut.wait();
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
    enqueue(report);
}

void EventBus::dispatchEvent(const Event& event) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, MarketEvent>) {
            // Safely extract the symbol from the variant
            std::string symbol = std::visit([](auto&& e) { return e.symbol; }, arg);
            // I need to evaluate if this copy is realy needed  
            std::map<uint64_t, MarketEventCallback> localCallbacks;
            std::map<uint64_t, MarketEventCallback> wildcardCallbacks;
            {
                std::lock_guard<std::mutex> lock(subMtx);
                // Get wildcard listeners (like the MockExecutionEngine)
                auto it_wild = marketEventSubscribers.find("*");
                if (it_wild != marketEventSubscribers.end()) {
                    wildcardCallbacks = it_wild->second;
                }
                // Get specific symbol listeners
                auto it = marketEventSubscribers.find(symbol);
                if (it != marketEventSubscribers.end()) {
                    localCallbacks = it->second;
                }
            }
            // Execute specific callbacks
            for (const auto& [id, callback] : localCallbacks) callback(arg);
            // Execute wildcard callbacks
            for (const auto& [id, callback] : wildcardCallbacks) callback(arg);
            
        } 
        else if constexpr (std::is_same_v<T, ExecutionReport>) {
            std::map<uint64_t, ExecutionCallback> localExecSubs;
            {
                std::lock_guard<std::mutex> lock(subMtx);
                localExecSubs = executionSubscribers;
            }

            for (const auto& [id, callback] : localExecSubs) {
                callback(arg);
            }
        }
        else if constexpr (std::is_same_v<T, Task>) {
            arg();
        }
    }, event);
}

void EventBus::processEvents() {
    Event event;
    
    // Main live processing loop
    while (isRunning) {
        // Block for up to 100ms waiting for an event. 
        // This drops CPU usage to 0% while idle, but still wakes up 
        // periodically to check if isRunning is false during shutdown.
        if (queue.wait_dequeue_timed(event, std::chrono::milliseconds(100))) {
            dispatchEvent(event);
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

}