#include <execution/OrderManager.hpp>
#include <core/Logger.hpp>

namespace bot {

OrderManager::OrderManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine)
    : eventBus_(std::move(eventBus)), engine_(std::move(engine)) {
    
    eventBus_->subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });

    syncWithExchange();

    // Start the background monitor thread
    isRunning_ = true;
    monitorThread_ = std::thread(&OrderManager::monitorLoop, this);
    LOG_INFO("[OrderManager] Active timeout monitor started.");
}

OrderManager::~OrderManager() {
    isRunning_ = false;
    cv_.notify_all();
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

// Intercepts the order, applies the fractional DAY workaround, and tracks the timeout
std::string OrderManager::submitOrder(OrderRequest request, std::chrono::milliseconds timeout) {
    // 1. Force TIF to DAY to bypass fractional share limitations on IOC
    request.tif = TimeInForce::DAY;
    
    // 2. Submit to the exchange
    std::string clientId = engine_->placeOrder(request);
    
    if (!clientId.empty()) {
        auto expiration = std::chrono::steady_clock::now() + timeout;
        
        std::lock_guard<std::mutex> lock(mtx_);
        // Pre-allocate the state so we know about it immediately
        ordersByClientId_[clientId].client_order_id = clientId;
        ordersByClientId_[clientId].status = OrderStatus::NEW;
        
        // Track the expiration
        expiriesByClientId_[clientId] = expiration;
        cv_.notify_one(); // Wake up the monitor thread
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
    state.symbol = report.symbol;
    state.side = report.side;
    state.status = report.status;
    
    exchangeToClientId_[report.order_id] = clientOrderId;
    
    LOG_DEBUG("[OrderManager] Updated Order: ClientID={}, Status={}", clientOrderId, (int)report.status);
    
    // If the order reached a terminal state, stop tracking it for timeouts
    if (report.status == OrderStatus::FILLED || 
        report.status == OrderStatus::CANCELED || 
        report.status == OrderStatus::REJECTED || 
        report.status == OrderStatus::EXPIRED) {
        
        expiriesByClientId_.erase(clientOrderId);
    }
}

void OrderManager::monitorLoop() {
    while (isRunning_) {
        std::unique_lock<std::mutex> lock(mtx_);
        
        if (expiriesByClientId_.empty()) {
            // Nothing to track, sleep until notified
            cv_.wait(lock, [this] { return !isRunning_ || !expiriesByClientId_.empty(); });
        } else {
            // Find the closest expiration time
            auto now = std::chrono::steady_clock::now();
            auto next_expiry = expiriesByClientId_.begin()->second;
            
            for (const auto& [cid, exp] : expiriesByClientId_) {
                if (exp < next_expiry) {
                    next_expiry = exp;
                }
            }
            
            // Sleep until the closest expiration time
            if (now < next_expiry) {
                cv_.wait_until(lock, next_expiry, [this] { return !isRunning_; });
            }
            
            // We woke up (either timed out or notified to shut down). Re-calculate 'now'.
            if (!isRunning_) break;
            now = std::chrono::steady_clock::now();
            
            // Scan for and cancel expired orders
            for (auto it = expiriesByClientId_.begin(); it != expiriesByClientId_.end(); ) {
                if (now >= it->second) {
                    std::string cid = it->first;
                    auto orderIt = ordersByClientId_.find(cid);
                    
                    if (orderIt != ordersByClientId_.end() && !orderIt->second.order_id.empty()) {
                        LOG_WARN("[OrderManager] Order {} timed out! Sending CANCEL.", cid);
                        engine_->cancelOrder(orderIt->second.order_id);
                    }
                    
                    // Remove from tracking so we don't spam cancels
                    it = expiriesByClientId_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
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
                  state.client_order_id, state.order_id, static_cast<int>(state.status));

        // Efficient insertions using std::move
        // We copy clientIdKey for the second map, but move it for the first
        new_exchangeToClientId.emplace(std::move(exchangeIdKey), clientIdKey);
        new_ordersByClientId.emplace(std::move(clientIdKey), std::move(state));
    }

    // --- LOG THE FINAL STATE BEFORE LOCKING ---
    LOG_INFO("[OrderManager] === Sync Complete. Current State Summary ===");
    LOG_INFO("[OrderManager] Total tracked orders: {}", new_ordersByClientId.size());
    
    for (const auto& [clientId, state] : new_ordersByClientId) {
        std::string sideLog = (state.side == Side::BUY) ? "BUY" : "SELL";
        LOG_INFO("  -> ClientID: {} | ExchID: {} | Symbol: {} | Side: {} | Qty: {:.4f} | Price: {:.2f}", 
                 clientId, state.order_id, state.symbol, sideLog, state.quantity, state.price);
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
