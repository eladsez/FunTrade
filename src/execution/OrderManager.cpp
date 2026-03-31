#include <execution/OrderManager.hpp>
#include <core/Logger.hpp>
#include <utils/magic_enum.hpp>

namespace bot {

OrderManager::OrderManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine)
    : eventBus_(std::move(eventBus)), engine_(std::move(engine)) {
    
    eventBus_->subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });

    syncWithExchange();
}

std::string OrderManager::submitOrder(OrderRequest request) {
    // Submit directly to the exchange
    std::string clientId = engine_->placeOrder(request);
    
    if (!clientId.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        // Pre-allocate the state so we know about it immediately
        OrderState& state = ordersByClientId_[clientId];
        state.client_order_id = clientId;
        state.symbol = request.symbol;
        state.side = request.side;
        state.type = request.type;
        state.quantity = request.quantity;
        state.filled_quantity = 0.0;
        state.status = OrderStatus::NEW;
    }
    
    return clientId;
}

void OrderManager::cancelOrder(const std::string& clientOrderId) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = ordersByClientId_.find(clientOrderId);
    if (it != ordersByClientId_.end() && !it->second.order_id.empty()) {
        engine_->cancelOrder(it->second.order_id);
    }
}

void OrderManager::cancelAllOrders() {
    engine_->cancelAllOrders();
    // We don't manually clear the maps here; we wait for the CANCELED execution reports
}

void OrderManager::onExecution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    std::string clientOrderId = report.client_order_id;
    if (clientOrderId.empty()) {
        auto it = exchangeToClientId_.find(report.order_id);
        if (it != exchangeToClientId_.end()) {
            clientOrderId = it->second;
        }
    }
    
    if (clientOrderId.empty()) {
        clientOrderId = "unknown_" + report.order_id;
    }

    OrderState& state = ordersByClientId_[clientOrderId];
    
    state.order_id = report.order_id;
    state.client_order_id = clientOrderId;
    
    // Only set symbol and side if they are empty (they should be populated from submitOrder)
    if (state.symbol.empty() && !report.symbol.empty()) state.symbol = report.symbol;
    
    state.status = report.status;
    
    // Safely update the cumulative filled quantity and average price
    if (report.cumulative_quantity > 0.0) {
        state.filled_quantity = report.cumulative_quantity;
    }
    if (report.avg_price > 0.0) {
        state.avg_price = report.avg_price;
    }
    
    exchangeToClientId_[report.order_id] = clientOrderId;

    LOG_INFO("[OrderManager] Updated Order: ClientID: {} | ExchID: {} | Symbol: {} | Side: {} | Status={} | Qty: {:.4f} | Price: {:.2f}", 
                 clientOrderId, state.order_id, state.symbol, std::string(magic_enum::enum_name(state.side)), 
                 std::string(magic_enum::enum_name(state.status)), state.quantity, state.price);
}

void OrderManager::syncWithExchange() {
    LOG_INFO("[OrderManager] Syncing open orders with exchange...");
    std::vector<OrderState> currentOrders = engine_->getOpenOrders();
    
    // 1. Build the new maps offline without any locks
    std::unordered_map<std::string, OrderState> new_ordersByClientId;
    std::unordered_map<std::string, std::string> new_exchangeToClientId;
    
    // Pre-allocate buckets to prevent rehashing during insertion
    new_ordersByClientId.reserve(currentOrders.size());
    new_exchangeToClientId.reserve(currentOrders.size());
    
    for (auto& state : currentOrders) {
        if (state.client_order_id.empty()) {
            continue;
        }
        
        // Extract keys before moving the state object
        std::string clientIdKey = state.client_order_id;
        std::string exchangeIdKey = state.order_id;
        
        LOG_DEBUG("[OrderManager] Synced Order: ClientID={}, ExchangeID={}, Status={}", 
                  state.client_order_id, state.order_id, std::string(magic_enum::enum_name(state.status)));

        // Efficient insertions using std::move
        // We copy clientIdKey for the second map, but move it for the first
        new_exchangeToClientId.emplace(std::move(exchangeIdKey), clientIdKey);
        new_ordersByClientId.emplace(std::move(clientIdKey), std::move(state));
    }

    // --- LOG THE FINAL STATE BEFORE LOCKING ---
    LOG_INFO("[OrderManager] === Sync Complete. Current State Summary ===");
    LOG_INFO("[OrderManager] Total tracked orders: {}", new_ordersByClientId.size());
    
    for (const auto& [clientId, state] : new_ordersByClientId) {
        LOG_INFO("  -> ClientID: {} | ExchID: {} | Symbol: {} | Side: {} | Status={} | Qty: {:.4f} | Price: {:.2f}", 
                 clientId, state.order_id, state.symbol, std::string(magic_enum::enum_name(state.side)), 
                 std::string(magic_enum::enum_name(state.status)), state.quantity, state.price);
    }
    LOG_INFO("[OrderManager] ============================================");

    // 2. Lock the mutex strictly for the double pointer swap (O(1))
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ordersByClientId_.swap(new_ordersByClientId);
        exchangeToClientId_.swap(new_exchangeToClientId);
    }
    // The lock is released instantly. The old map data is destroyed here.
}

std::optional<OrderState> OrderManager::getOrder(const std::string& clientOrderId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = ordersByClientId_.find(clientOrderId);
    if (it != ordersByClientId_.end()) return it->second;
    return std::nullopt;
}

std::vector<OrderState> OrderManager::getOpenOrders() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<OrderState> openOrders;
    for (const auto& [cid, state] : ordersByClientId_) {
        if (state.status != OrderStatus::FILLED && state.status != OrderStatus::CANCELED && state.status != OrderStatus::REJECTED) {
            openOrders.push_back(state);
        }
    }
    return openOrders;
}

} // namespace bot
