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
    nlohmann::json ordersJson = engine_->getOpenOrders();
    
    std::lock_guard<std::mutex> lock(mtx_);
    ordersByClientId_.clear();
    exchangeToClientId_.clear();
    
    if (ordersJson.is_array()) {
        for (const auto& oJson : ordersJson) {
            try {
                OrderState state;
                state.order_id = oJson.value("id", "");
                state.client_order_id = oJson["client_order_id"].is_null() ? "" : oJson.value("client_order_id", "");
                state.symbol = oJson.value("symbol", "");
                
                std::string qtyStr = oJson["qty"].is_null() ? "0" : oJson.value("qty", "0");
                state.quantity = std::stod(qtyStr);

                std::string priceStr = oJson["limit_price"].is_null() ? "0" : oJson.value("limit_price", "0");
                state.price = std::stod(priceStr);

                std::string sideStr = oJson.value("side", "buy");
                state.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
                
                std::string typeStr = oJson.value("type", "market");
                if (typeStr == "limit") state.type = OrderType::LIMIT;
                else if (typeStr == "trailing_stop") state.type = OrderType::TRAILING_STOP;
                else state.type = OrderType::MARKET;
                
                std::string statusStr = oJson.value("status", "new");
                if (statusStr == "filled") state.status = OrderStatus::FILLED;
                else if (statusStr == "partially_filled") state.status = OrderStatus::PARTIALLY_FILLED;
                else if (statusStr == "canceled") state.status = OrderStatus::CANCELED;
                else state.status = OrderStatus::NEW;
                
                if (!state.client_order_id.empty()) {
                    ordersByClientId_[state.client_order_id] = state;
                    exchangeToClientId_[state.order_id] = state.client_order_id;
                    LOG_INFO("[OrderManager] Synced Order: ClientID={}, ExchangeID={}, Status={}", 
                             state.client_order_id, state.order_id, statusStr);
                }
            } catch (const std::exception& e) {
                LOG_ERROR("[OrderManager] Error parsing synced order: {}", e.what());
            }
        }
    }

    // --- LOG THE FINAL STATE AFTER SYNC ---
    LOG_INFO("[OrderManager] === Sync Complete. Current State Summary ===");
    LOG_INFO("[OrderManager] Total tracked orders: {}", ordersByClientId_.size());
    
    for (const auto& [clientId, state] : ordersByClientId_) {
        // Convert the enum Side back to string for logging
        std::string sideLog = (state.side == Side::BUY) ? "BUY" : "SELL";
        
        LOG_INFO("  -> ClientID: {} | ExchID: {} | Symbol: {} | Side: {} | Qty: {:.4f} | Price: {:.2f}", 
                 clientId, state.order_id, state.symbol, sideLog, state.quantity, state.price);
    }
    LOG_INFO("[OrderManager] ============================================");
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
