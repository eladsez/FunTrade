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
    ~OrderManager() = default;

    // Callbacks
    void onExecution(const ExecutionReport& report);

    // New Exec API
    std::string submitOrder(OrderRequest request);
    void cancelOrder(const std::string& clientOrderId);
    void cancelAllOrders();

    // Getters
    std::optional<OrderState> getOrder(const std::string& clientOrderId) const;
    std::vector<OrderState> getOpenOrders() const;

private:
    void syncWithExchange();
    
    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> engine_;
    
    std::unordered_map<std::string, OrderState> ordersByClientId_;
    std::unordered_map<std::string, std::string> exchangeToClientId_;
        
    mutable std::mutex mtx_;
};

} // namespace bot