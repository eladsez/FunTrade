#include <execution/IbkrExecutionEngine.hpp>
#include <core/Logger.hpp>

// TWS API Includes
#include <Order.h>
#include <OrderCancel.h> // Required for newer TWS APIs
#include <Contract.h>
#include <Execution.h>
#include <Decimal.h> 
#include <OrderState.h>
#include <Execution.h>

#include <chrono>

namespace bot {

IbkrExecutionEngine::IbkrExecutionEngine(std::shared_ptr<IbkrInfra> infra)
    : infra_(std::move(infra)) {}

bool IbkrExecutionEngine::connectToInfra() {
    LOG_INFO("[IbkrExecutionEngine] Hooking up execution callbacks to Infra...");
    
    infra_->setNextValidIdCallback(
        [this](int id) { this->onNextValidId(id); }
    );

    infra_->setOrderStatusCallback(
        [this](OrderId orderId, const std::string& status, Decimal filled, Decimal /*remaining*/,
               double avgFillPrice, long long /*permId*/, int /*parentId*/, double /*lastFillPrice*/,
               int /*clientId*/, const std::string& /*whyHeld*/, double /*mktCapPrice*/) {
            this->onOrderStatus(static_cast<int>(orderId), status, filled, avgFillPrice);
        }
    );

    infra_->setExecDetailsCallback(
        [this](int reqId, const Contract& contract, const Execution& execution) {
            this->onExecDetails(reqId, contract, execution);
        }
    );

    infra_->setUpdateAccountValueCallback(
        [this](const std::string& k, const std::string& v, const std::string& c, const std::string& a) {
            this->onUpdateAccountValue(k, v, c, a);
        }
    );

    infra_->setPositionCallback(
        [this](const std::string& a, const Contract& c, Decimal p, double ac) {
            this->onPosition(a, c, p, ac);
        }
    );

    infra_->setOpenOrderCallback(
        [this](OrderId id, const Contract& c, const ::Order& o, const ::OrderState& os) {
            this->onOpenOrder(id, c, o, os);
        }
    );

    infra_->setErrorCallback(
        [this](int id, int errorCode, const std::string& errorString, const std::string& advancedJson) {
            this->onError(id, errorCode, errorString, advancedJson);
        }
    );

    infra_->setExecDetailsEndCallback(
        [this](int /*reqId*/) {
            std::lock_guard<std::mutex> lock(syncMutex_);
            isExecutionsSynced_ = true;
            syncCv_.notify_all();
        }
    );

    infra_->setAccountDownloadEndCallback(
        [this](const std::string& accountName) { 
            this->onAccountDownloadEnd(accountName); 
        }
    );

    infra_->setPositionEndCallback(
        [this]() { 
            this->onPositionEnd(); 
        }
    );

    infra_->setOpenOrderEndCallback(
        [this]() { 
            this->onOpenOrderEnd(); 
        }
    );

    infra_->addReconnectCallback(
        [this]() {
            this->onReconnect(); 
        }
    );

    if (infra_->client().connected()) {
        infra_->client()->reqIds(-1); 
        infra_->client()->reqAccountUpdates(true, ""); 
        infra_->client()->reqPositions();
        infra_->client()->reqAllOpenOrders();
        // Today's executions: books fills that happened while the bot was down (deduped by execId).
        infra_->client()->reqExecutions(kExecutionsReqId, ExecutionFilter());
    }
    else {
        LOG_ERROR("[IbkrExecutionEngine] Failed to connect to IbkrInfra");
    }

    // THE BARRIER: Wait for TWS to finish downloading the state
    LOG_DEBUG("[IbkrExecutionEngine] Waiting for initial account, position, and order sync from TWS...");
    std::unique_lock<std::mutex> lock(syncMutex_);
    
    // Wait up to 10 seconds for the sync to complete
    bool synced = syncCv_.wait_for(lock, std::chrono::seconds(10), [this] {
        return isAccountSynced_ && isPositionsSynced_ && isOrdersSynced_ && isExecutionsSynced_;
    });

    if (!synced) {
        LOG_ERROR("[IbkrExecutionEngine] Timeout waiting for TWS state sync. Proceeding with partial/empty state.");
        // Depending on your risk tolerance, you might want to return false here to crash the bot safely.
    } else {
        LOG_DEBUG("[IbkrExecutionEngine] Successfully synced all initial state from TWS.");
    }

    return true;
}

void IbkrExecutionEngine::disconnectFromInfra() {
    LOG_DEBUG("[IbkrExecutionEngine] Disconnecting ExecutionEngine from Infra.");
}

void IbkrExecutionEngine::onReconnect() {
    LOG_INFO("[IbkrExecutionEngine] Re-requesting account, positions, and open orders after reconnect...");
    
    if (infra_->client().connected()) {
        infra_->client()->reqAccountUpdates(true, ""); 
        infra_->client()->reqPositions();
        infra_->client()->reqAllOpenOrders();
        infra_->client()->reqExecutions(kExecutionsReqId, ExecutionFilter());  // fills missed while down
    }
}

// =========================================================================
// Order Management
// =========================================================================

std::string IbkrExecutionEngine::placeOrder(const OrderRequest& request) {
    if (!infra_->client().connected()) {
        LOG_ERROR("[IbkrExecutionEngine] Cannot place order. Not connected to TWS.");
        return "";
    }
    if (request.client_order_id.empty()) {
        LOG_ERROR("[IbkrExecutionEngine] Refusing order without client_order_id for {}", request.symbol);
        return "";
    }

    int ibkrOrderId = 0;
    {
        std::unique_lock<std::mutex> lock(idMutex_);
        // Bounded: this runs on the event dispatcher, so an unbounded wait would freeze every strategy.
        if (!idCv_.wait_for(lock, kOrderIdTimeout, [this] { return validIdReceived_; })) {
            LOG_ERROR("[IbkrExecutionEngine] No valid order id from TWS after {}s. Order {} not sent.",
                      kOrderIdTimeout.count(), request.client_order_id);
            return "";
        }
        ibkrOrderId = nextValidId_++;
    }

    // Track before sending: TWS can answer before placeOrder() returns.
    tracker_.onPlaced(ibkrOrderId, request);

    Contract contract = createContract(request.symbol);

    ::Order order; 
    order.orderId = ibkrOrderId;
    order.orderRef = request.client_order_id;  // survives restarts; lets us re-attribute fills
    
    order.action = (request.side == Side::BUY) ? "BUY" : "SELL";
    order.totalQuantity = DecimalFunctions::doubleToDecimal(request.quantity);
    order.tif = translateTimeInForce(request.tif);

    switch (request.type) {
        case OrderType::MARKET:
            order.orderType = "MKT";
            break;
        case OrderType::LIMIT:
            order.orderType = "LMT";
            order.lmtPrice = request.price;
            break;
        case OrderType::STOP:
            order.orderType = "STP";
            order.auxPrice = request.stop_price; 
            break;
        case OrderType::STOP_LIMIT:
            order.orderType = "STP LMT";
            order.lmtPrice = request.price;
            order.auxPrice = request.stop_price;
            break;
        case OrderType::TRAILING_STOP:
            order.orderType = "TRAIL";
            if (request.trail_percent > 0) {
                order.trailingPercent = request.trail_percent;
            } else {
                order.auxPrice = request.trail_price; 
            }
            break;
        case OrderType::MARKET_ON_CLOSE:
            // Fills in the closing auction. NYSE/Nasdaq stop accepting MOC orders ~15:50 ET.
            order.orderType = "MOC";
            order.tif = "DAY";
            break;
    }

    LOG_DEBUG("[IbkrExecutionEngine] Placing {} Order {} for {} {} (ID: {}, Ref: {})", 
             order.orderType, order.action, DecimalFunctions::decimalToDouble(order.totalQuantity), 
             request.symbol, order.orderId, order.orderRef);

    infra_->client()->placeOrder(ibkrOrderId, contract, order);
    return request.client_order_id;
}

bool IbkrExecutionEngine::cancelOrder(const std::string& order_id) {
    std::optional<long> ibkrOrderId = tracker_.orderIdFor(order_id);
    if (!ibkrOrderId) {
        // Not a client id we know: accept a plain numeric IBKR order id.
        char* end = nullptr;
        long parsed = std::strtol(order_id.c_str(), &end, 10);
        if (order_id.empty() || *end != '\0') {
            LOG_WARN("[IbkrExecutionEngine] Invalid cancel request. Could not parse or find ID: {}", order_id);
            return false;
        }
        ibkrOrderId = parsed;
    }

    if (!infra_->client().connected()) {
        LOG_ERROR("[IbkrExecutionEngine] Cannot cancel order {}. Not connected to TWS.", *ibkrOrderId);
        return false;
    }
    LOG_INFO("[IbkrExecutionEngine] Requesting cancellation for IBKR Order ID: {}", *ibkrOrderId);
    ::OrderCancel cancelReq; // Required struct for newer TWS API
    infra_->client()->cancelOrder(*ibkrOrderId, cancelReq);
    return true;
}

bool IbkrExecutionEngine::cancelAllOrders() {
    if (infra_->client().connected()) {
        LOG_INFO("[IbkrExecutionEngine] Requesting Global Cancel.");
        
        ::OrderCancel cancelReq; // Instantiate the required struct
        infra_->client()->reqGlobalCancel(cancelReq); 
        
        return true;
    }
    return false;
}

// =========================================================================
// Callbacks
// =========================================================================

void IbkrExecutionEngine::onNextValidId(int orderId) {
    std::lock_guard<std::mutex> lock(idMutex_);
    nextValidId_ = orderId;
    validIdReceived_ = true;
    LOG_INFO("[IbkrExecutionEngine] Received NextValidId: {}", orderId);
    idCv_.notify_all(); 
}

void IbkrExecutionEngine::emit(const std::optional<ExecutionReport>& report) {
    if (report && reportCb_) reportCb_(*report);
}

void IbkrExecutionEngine::onOrderStatus(int orderId, const std::string& status, Decimal filled, double avgFillPrice) {
    double filledQty = (filled == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(filled);
    emit(tracker_.onOrderStatus(orderId, status, filledQty, avgFillPrice));
}

void IbkrExecutionEngine::onExecDetails(int reqId, const Contract& contract, const Execution& execution) {
    (void)reqId;
    double shares = (execution.shares == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(execution.shares);
    emit(tracker_.onExecution(execution.orderId, execution.orderRef, execution.execId, contract.symbol,
                              execution.side, shares, execution.price, execution.avgPrice));
}

std::optional<double> IbkrExecutionEngine::toUsd(const std::optional<double>& baseValue) {
    if (!baseValue || !usdRate_ || *usdRate_ <= 0.0) return std::nullopt;
    return *baseValue / *usdRate_;
}

void IbkrExecutionEngine::onUpdateAccountValue(const std::string& key, const std::string& val, const std::string& currency, const std::string& accountName) {
    (void)accountName;
    try {
        std::lock_guard<std::mutex> lock(accountMutex_);
        if (key == "AvailableFunds" && currency != "BASE") {
            baseAvailableFunds_ = std::stod(val);
        } else if (key == "NetLiquidation" && currency != "BASE") {
            baseNetLiquidation_ = std::stod(val);
        } else if (key == "ExchangeRate" && currency == "USD") {
            usdRate_ = std::stod(val);
        }
    } catch (const std::exception& e) {
        LOG_WARN("[IbkrExecutionEngine] Failed to parse account value for {}: {}", key, val);
    }
}

void IbkrExecutionEngine::onPosition(const std::string& account, const Contract& contract, Decimal pos, double avgCost) {
    (void)account;
    std::lock_guard<std::mutex> lock(positionMutex_);
    
    double qty = (pos == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(pos);

    // If position drops to 0, remove it from the open positions map
    if (qty == 0.0) {
        openPositions_.erase(contract.symbol);
    } else {
        Position p;
        p.symbol = contract.symbol;
        p.quantity = qty;       
        p.avg_price = avgCost; 
        
        // Note: The 'position' callback does not provide current_price or realized_pnl.
        // These will default to 0.0. Typically, your PortfolioManager updates the 
        // current_price independently by listening to the EventBus market data ticks.
        
        openPositions_[contract.symbol] = p;
    }
}

void IbkrExecutionEngine::onOpenOrder(OrderId orderId, const Contract& contract, const ::Order& order, const ::OrderState& ibkrOrderState) {
    OrderState state; // This resolves safely to bot::OrderState
    state.order_id = std::to_string(orderId);
    state.client_order_id = order.orderRef;
    state.symbol = contract.symbol;
    state.side = (order.action == "BUY") ? Side::BUY : Side::SELL;
    
    // Map OrderType
    if (order.orderType == "MKT") state.type = OrderType::MARKET;
    else if (order.orderType == "LMT") state.type = OrderType::LIMIT;
    else if (order.orderType == "STP") state.type = OrderType::STOP;
    else if (order.orderType == "STP LMT") state.type = OrderType::STOP_LIMIT;
    else if (order.orderType == "TRAIL") state.type = OrderType::TRAILING_STOP;
    else if (order.orderType == "MOC") state.type = OrderType::MARKET_ON_CLOSE;
    else state.type = OrderType::MARKET; 

    state.quantity = (order.totalQuantity == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(order.totalQuantity);
    
    // Map Price
    if (state.type == OrderType::LIMIT || state.type == OrderType::STOP_LIMIT) {
        state.price = (order.lmtPrice == UNSET_DOUBLE) ? 0.0 : order.lmtPrice;
    } else if (state.type == OrderType::STOP) {
        state.price = (order.auxPrice == UNSET_DOUBLE) ? 0.0 : order.auxPrice;
    } else {
        state.price = 0.0; 
    }

    state.status = IbkrOrderTracker::parseStatus(ibkrOrderState.status);

    // Orders from a previous run: track them so their fills are attributed via orderRef.
    tracker_.onOpenOrder(orderId, order.orderRef, contract.symbol, state.side, state.quantity);

    std::lock_guard<std::mutex> lock(orderMutex_);
    openOrders_[orderId] = state; 
}

// =========================================================================
// Helpers & Synchronous Methods
// =========================================================================

Contract IbkrExecutionEngine::createContract(const std::string& symbol) {
    Contract contract;
    contract.symbol = symbol;
    contract.secType = "STK";
    contract.currency = "USD";
    contract.exchange = "SMART"; 
    return contract;
}

std::string IbkrExecutionEngine::translateTimeInForce(TimeInForce tif) {
    switch(tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::DAY: return "DAY";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        default: return "GTC";
    }
}

// Both in USD (see toUsd); nullopt until TWS has sent the value and the USD exchange rate.
std::optional<double> IbkrExecutionEngine::getAvailableCash() {
    std::lock_guard<std::mutex> lock(accountMutex_);
    return toUsd(baseAvailableFunds_);
}

std::optional<double> IbkrExecutionEngine::getPortfolioValue() {
    std::lock_guard<std::mutex> lock(accountMutex_);
    return toUsd(baseNetLiquidation_);
}

std::vector<Position> IbkrExecutionEngine::getOpenPositions() {
    std::lock_guard<std::mutex> lock(positionMutex_);
    std::vector<Position> result;
    result.reserve(openPositions_.size());
    
    for (const auto& [symbol, pos] : openPositions_) {
        result.push_back(pos);
    }
    return result;
}

std::vector<OrderState> IbkrExecutionEngine::getOpenOrders() {
    std::lock_guard<std::mutex> lock(orderMutex_);
    std::vector<OrderState> result;
    result.reserve(openOrders_.size());
    
    for (const auto& [id, state] : openOrders_) {
        if (!isTerminal(state.status)) result.push_back(state);
    }
    return result;
}

void IbkrExecutionEngine::onError(int reqId, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) {
    if (!tracker_.isTracked(reqId)) return;  // not about one of our orders

    if (!advancedOrderRejectJson.empty()) {
        LOG_WARN("[IbkrExecutionEngine] Order {} error {}: {} | Advanced Info: {}", reqId, errorCode, errorString, advancedOrderRejectJson);
    } else {
        LOG_WARN("[IbkrExecutionEngine] Order {} error {}: {}", reqId, errorCode, errorString);
    }
    emit(tracker_.onError(reqId, errorCode));
}

void IbkrExecutionEngine::onAccountDownloadEnd(const std::string& accountName) {
    (void)accountName;
    std::lock_guard<std::mutex> lock(syncMutex_);
    isAccountSynced_ = true;
    syncCv_.notify_all();
}

void IbkrExecutionEngine::onPositionEnd() {
    std::lock_guard<std::mutex> lock(syncMutex_);
    isPositionsSynced_ = true;
    syncCv_.notify_all();
}

void IbkrExecutionEngine::onOpenOrderEnd() {
    std::lock_guard<std::mutex> lock(syncMutex_);
    isOrdersSynced_ = true;
    syncCv_.notify_all();
}

} // namespace bot