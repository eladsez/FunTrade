#pragma once
#include <string>
#include <map>
#include <mutex>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>

namespace bot {

struct OrderState {
    std::string order_id;
    std::string client_order_id;
    std::string symbol;
    Side side;
    OrderType type;
    double quantity;
    double price;
    OrderStatus status;
};

class OrderManager {
public:
    OrderManager(IEventBus& eventBus, IExecutionEngine& engine);
    ~OrderManager() = default;

    void syncWithExchange();
    
    // Callbacks
    void onExecution(const ExecutionReport& report);

    // Getters
    std::optional<OrderState> getOrder(const std::string& clientOrderId) const;
    std::vector<OrderState> getOpenOrders() const;

private:
    IEventBus& eventBus_;
    IExecutionEngine& engine_;
    
    std::map<std::string, OrderState> ordersByClientId_;
    std::map<std::string, std::string> exchangeToClientId_;
    mutable std::mutex mtx_;
};

} // namespace bot
