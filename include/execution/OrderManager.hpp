#pragma once
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>

namespace bot {


class OrderManager {
public:
    OrderManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine);
    ~OrderManager();

    // Callbacks
    void onExecution(const ExecutionReport& report);

    // New Exec API
    std::string submitOrder(OrderRequest request, std::chrono::milliseconds timeout);
    void cancelOrder(const std::string& clientOrderId);
    void cancelAllOrders();

    // Getters
    std::optional<OrderState> getOrder(const std::string& clientOrderId) const;
    std::vector<OrderState> getOpenOrders() const;

private:
    void syncWithExchange();
    void monitorLoop(); // Background thread for timeouts
    
    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> engine_;
    
    std::unordered_map<std::string, OrderState> ordersByClientId_;
    std::unordered_map<std::string, std::string> exchangeToClientId_;
    
    // Tracks when each order should expire
    std::map<std::string, std::chrono::steady_clock::time_point> expiriesByClientId_;
    
    mutable std::mutex mtx_;

    // Threading controls
    std::atomic<bool> isRunning_{false};
    std::thread monitorThread_;
    std::condition_variable cv_;
};

} // namespace bot