#include <execution/OrderManager.hpp>
#include <core/Logger.hpp>

namespace bot {

OrderManager::OrderManager(IEventBus& eventBus, IExecutionEngine& engine)
    : eventBus_(eventBus), engine_(engine) {
    
    eventBus_.subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });
}

void OrderManager::syncWithExchange() {
    LOG_INFO("[OrderManager] Syncing open orders with exchange...");
    nlohmann::json ordersJson = engine_.getOpenOrders();
    
    std::lock_guard<std::mutex> lock(mtx_);
    ordersByClientId_.clear();
    exchangeToClientId_.clear();
    
    if (ordersJson.is_array()) {
        for (const auto& oJson : ordersJson) {
            try {
                OrderState state;
                state.order_id = oJson.value("id", "");
                state.client_order_id = oJson.value("client_order_id", "");
                state.symbol = oJson.value("symbol", "");
                
                std::string sideStr = oJson.value("side", "buy");
                state.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
                
                std::string typeStr = oJson.value("type", "market");
                if (typeStr == "limit") state.type = OrderType::LIMIT;
                else if (typeStr == "trailing_stop") state.type = OrderType::TRAILING_STOP;
                else state.type = OrderType::MARKET;
                
                state.quantity = std::stod(oJson.value("qty", "0"));
                state.price = std::stod(oJson.value("limit_price", "0"));
                
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
        // Unknown order, create new state if it's a new order
        clientOrderId = "unknown_" + report.order_id;
    }

    OrderState& state = ordersByClientId_[clientOrderId];
    state.order_id = report.order_id;
    state.client_order_id = clientOrderId;
    state.symbol = report.symbol;
    state.side = report.side;
    state.status = report.status;
    // Note: quantity and price might need to be tracked from OrderRequest if not in Report
    
    exchangeToClientId_[report.order_id] = clientOrderId;
    
    LOG_INFO("[OrderManager] Updated Order: ClientID={}, Status={}", clientOrderId, (int)report.status);
    
    // Cleanup if closed
    if (report.status == OrderStatus::FILLED || report.status == OrderStatus::CANCELED || report.status == OrderStatus::REJECTED) {
        // We might want to keep them for a while, but for now just leave them in the map or move to history
    }
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
