#include <execution/OrderManager.hpp>
#include <core/Logger.hpp>
#include <utils/magic_enum.hpp>
#include <algorithm>

namespace bot {

OrderManager::OrderManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine)
    : eventBus_(std::move(eventBus)), engine_(std::move(engine)) {
    
    eventBus_->subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });

    syncWithExchange();
}

std::string OrderManager::submitOrder(OrderRequest request) {
    if (request.client_order_id.empty()) {
        request.client_order_id = OrderIdUtils::newClientOrderId(request.symbol);
    }
    const std::string clientId = request.client_order_id;

    // Register before sending: a fast exchange (or a synchronous engine) can report the
    // fill before placeOrder() even returns, and that report must find this state.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        OrderState& state = ordersByClientId_[clientId];
        state.client_order_id = clientId;
        state.symbol = request.symbol;
        state.side = request.side;
        state.type = request.type;
        state.price = request.price;
        state.quantity = request.quantity;
    }

    std::string placedId = engine_->placeOrder(request);

    if (placedId != clientId) {
        LOG_ERROR("[OrderManager] Placement failed for {} (engine returned '{}'). Marking REJECTED.", clientId, placedId);
        std::lock_guard<std::mutex> lock(mtx_);
        ordersByClientId_[clientId].status = OrderStatus::REJECTED;
        return "";
    }
    return clientId;
}

void OrderManager::cancelOrder(const std::string& clientOrderId) {
    std::string idForEngine;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = ordersByClientId_.find(clientOrderId);
        if (it == ordersByClientId_.end() || isTerminal(it->second.status)) return;
        // Before the first report we don't know the exchange ID yet; engines accept the client ID.
        idForEngine = it->second.order_id.empty() ? clientOrderId : it->second.order_id;
    }
    // Never call into the engine (possibly network I/O) while holding the lock: onExecution
    // runs on the event dispatcher and would stall every strategy.
    engine_->cancelOrder(idForEngine);
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
    
    if (!report.order_id.empty()) state.order_id = report.order_id;
    state.client_order_id = clientOrderId;
    
    // Only set symbol and side if they are empty (they should be populated from submitOrder)
    if (state.symbol.empty() && !report.symbol.empty()) state.symbol = report.symbol;
    if (state.side == Side::UNKNOWN) state.side = report.side;
    
    // Terminal states are final; reports can arrive late or duplicated.
    if (!isTerminal(state.status)) {
        state.status = report.status;
    }
    
    // Filled quantity only grows (cancel reports may carry 0).
    state.filled_quantity = std::max(state.filled_quantity, report.cumulative_quantity);
    if (report.avg_price > 0.0) {
        state.avg_price = report.avg_price;
    }
    
    if (!state.order_id.empty()) exchangeToClientId_[state.order_id] = clientOrderId;

    LOG_INFO("[OrderManager] Updated Order: ClientID: {} | ExchID: {} | Symbol: {} | Side: {} | Status={} | Filled: {:.4f}/{:.4f} | AvgPx: {:.2f}", 
                 clientOrderId, state.order_id, state.symbol, std::string(magic_enum::enum_name(state.side)), 
                 std::string(magic_enum::enum_name(state.status)), state.filled_quantity, state.quantity, state.avg_price);
}

void OrderManager::syncWithExchange() {
    std::vector<OrderState> brokerOrders = engine_->getOpenOrders();

    std::lock_guard<std::mutex> lock(mtx_);
    size_t added = 0;
    for (auto& state : brokerOrders) {
        if (state.client_order_id.empty()) continue;
        if (!state.order_id.empty()) exchangeToClientId_[state.order_id] = state.client_order_id;
        auto [it, inserted] = ordersByClientId_.try_emplace(state.client_order_id, state);
        if (inserted) added++;
    }
    LOG_INFO("[OrderManager] Synced with exchange: {} open orders at broker, {} new to the book.", brokerOrders.size(), added);
    for (const auto& s : brokerOrders) {
        LOG_INFO("  -> ClientID: {} | ExchID: {} | {} {} {:.4f} | Status={}", s.client_order_id, s.order_id,
                 std::string(magic_enum::enum_name(s.side)), s.symbol, s.quantity, std::string(magic_enum::enum_name(s.status)));
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
        if (!isTerminal(state.status)) {
            openOrders.push_back(state);
        }
    }
    return openOrders;
}

} // namespace bot
