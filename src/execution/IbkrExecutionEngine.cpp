#include <execution/IbkrExecutionEngine.hpp>
#include <core/Logger.hpp>

// TWS API Includes
#include <Order.h>
#include <OrderCancel.h> // Required for newer TWS APIs
#include <Contract.h>
#include <Execution.h>
#include <Decimal.h> 
#include <OrderState.h>

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
        [this](int orderId, const std::string& status, Decimal filled, Decimal remaining, 
               double avgFillPrice, int permId, int parentId, double lastFillPrice, 
               int clientId, const std::string& whyHeld, double mktCapPrice) {
            this->onOrderStatus(orderId, status, filled, remaining, avgFillPrice, permId, parentId, lastFillPrice, clientId, whyHeld, mktCapPrice);
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

    if (infra_->getClient() && infra_->getClient()->isConnected()) {
        infra_->getClient()->reqIds(-1); 
        infra_->getClient()->reqAccountUpdates(true, ""); 
        infra_->getClient()->reqPositions();
        infra_->getClient()->reqAllOpenOrders();
    }
    else {
        LOG_ERROR("[IbkrExecutionEngine] Failed to connect to IbkrInfra");
    }

    // THE BARRIER: Wait for TWS to finish downloading the state
    LOG_DEBUG("[IbkrExecutionEngine] Waiting for initial account, position, and order sync from TWS...");
    std::unique_lock<std::mutex> lock(syncMutex_);
    
    // Wait up to 10 seconds for the sync to complete
    bool synced = syncCv_.wait_for(lock, std::chrono::seconds(10), [this] {
        return isAccountSynced_ && isPositionsSynced_ && isOrdersSynced_;
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

// =========================================================================
// Order Management
// =========================================================================

std::string IbkrExecutionEngine::placeOrder(const OrderRequest& request) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) {
        LOG_ERROR("[IbkrExecutionEngine] Cannot place order. Not connected to TWS.");
        return "";
    }

    std::string uniqueClientId = OrderIdUtils::generateClientOrderId(request.client_order_id);

    int ibkrOrderId = 0;
    {
        std::unique_lock<std::mutex> lock(idMutex_);
        idCv_.wait(lock, [this]{ return validIdReceived_; });
        
        ibkrOrderId = nextValidId_++;
        clientRefToOrderId_[uniqueClientId] = ibkrOrderId;
        orderIdToClientRef_[ibkrOrderId] = uniqueClientId;
    }

    Contract contract = createContract(request.symbol);

    ::Order order; 
    order.orderId = ibkrOrderId;
    order.orderRef = uniqueClientId; 
    
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
    }

    LOG_DEBUG("[IbkrExecutionEngine] Placing {} Order {} for {} {} (ID: {}, Ref: {})", 
             order.orderType, order.action, DecimalFunctions::decimalToDouble(order.totalQuantity), 
             request.symbol, order.orderId, order.orderRef);

    infra_->getClient()->placeOrder(ibkrOrderId, contract, order);
    // return request.client_order_id and not uniqueClientId because
    // the unique exist only in the execution level (for the exchange to get unique for sure)
    return request.client_order_id;
}

bool IbkrExecutionEngine::cancelOrder(const std::string& order_id) {
    int ibkrOrderId = -1;

    // 1. Try to parse it directly as the IBKR Exchange ID (integer)
    try {
        ibkrOrderId = std::stoi(order_id);
    } catch (const std::invalid_argument&) {
        // 2. Fallback: If it's not an integer, check if it's a client_order_id reference
        std::lock_guard<std::mutex> lock(idMutex_);
        auto it = clientRefToOrderId_.find(order_id);
        
        if (it != clientRefToOrderId_.end()) {
            ibkrOrderId = it->second;
        } else {
            LOG_WARN("[IbkrExecutionEngine] Invalid cancel request. Could not parse or find ID: {}", order_id);
            return false;
        }
    }

    // 3. Send the cancellation to TWS
    if (ibkrOrderId != -1) {
        LOG_INFO("[IbkrExecutionEngine] Requesting cancellation for IBKR Order ID: {}", ibkrOrderId);
        
        ::OrderCancel cancelReq; // Required struct for newer TWS API
        if (infra_->getClient() && infra_->getClient()->isConnected()) {
            infra_->getClient()->cancelOrder(ibkrOrderId, cancelReq); 
            return true;
        } else {
            LOG_ERROR("[IbkrExecutionEngine] Cannot cancel order {}. Not connected to TWS.", ibkrOrderId);
        }
    }
    
    return false;
}

bool IbkrExecutionEngine::cancelAllOrders() {
    if (infra_->getClient() && infra_->getClient()->isConnected()) {
        LOG_INFO("[IbkrExecutionEngine] Requesting Global Cancel.");
        
        ::OrderCancel cancelReq; // Instantiate the required struct
        infra_->getClient()->reqGlobalCancel(cancelReq); 
        
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

void IbkrExecutionEngine::onOrderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining, 
                                        double avgFillPrice, int permId, int parentId, double lastFillPrice, 
                                        int clientId, const std::string& whyHeld, double mktCapPrice) {
    
    (void)remaining; (void)permId; (void)parentId; (void)clientId; (void)whyHeld; (void)mktCapPrice;

    std::string clientRef;
    {
        std::lock_guard<std::mutex> lock(idMutex_);
        auto it = orderIdToClientRef_.find(orderId);
        if (it != orderIdToClientRef_.end()) {
            clientRef = it->second;
        } else {
            return; 
        }
    }

    ExecutionReport report;
    report.order_id = std::to_string(orderId);
    report.client_order_id = OrderIdUtils::extractBaseClientId(clientRef);;
    report.status = parseIbkrStatus(status);
    
    report.cumulative_quantity = (filled == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(filled);
    report.avg_price = avgFillPrice;
    report.last_price = lastFillPrice; 
    report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();

    if (reportCb_)
        reportCb_(report);
}

void IbkrExecutionEngine::onExecDetails(int reqId, const Contract& contract, const Execution& execution) {
    (void)reqId; 

    ExecutionReport report;
    report.order_id = std::to_string(execution.orderId);
    report.client_order_id = OrderIdUtils::extractBaseClientId(execution.orderRef);; 
    report.symbol = contract.symbol;
    report.side = (execution.side == "BOT") ? Side::BUY : Side::SELL;
    report.status = OrderStatus::FILLED; 
    
    // Adjusted fields to match DomainModels.hpp
    report.last_quantity = (execution.shares == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(execution.shares);
    report.cumulative_quantity = (execution.cumQty == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(execution.cumQty);
    report.last_price = execution.price;
    report.avg_price = execution.avgPrice;
    
    report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();

    if (reportCb_)
        reportCb_(report);
}

void IbkrExecutionEngine::onUpdateAccountValue(const std::string& key, const std::string& val, const std::string& currency, const std::string& accountName) {
    (void)accountName;
    
    // We usually only care about the base currency (e.g., USD) to avoid double-counting
    if (currency == "USD" || currency.empty()) {
        try {
            if (key == "AvailableFunds") { // Can also use "TotalCashValue"
                cachedCash_.store(std::stod(val));
                cashHasValue_.store(true);
            } else if (key == "NetLiquidation") { // Total portfolio value
                cachedPortfolioValue_.store(std::stod(val));
                portHasValue_.store(true);
            }
        } catch (const std::exception& e) {
            LOG_WARN("[IbkrExecutionEngine] Failed to parse account value for {}: {}", key, val);
        }
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
    std::lock_guard<std::mutex> lock(orderMutex_);
    
    OrderState state; // This resolves safely to bot::OrderState
    state.order_id = std::to_string(orderId);
    state.client_order_id = OrderIdUtils::extractBaseClientId(order.orderRef);
    state.symbol = contract.symbol;
    
    state.side = (order.action == "BUY") ? Side::BUY : Side::SELL;
    
    // Map OrderType
    if (order.orderType == "MKT") state.type = OrderType::MARKET;
    else if (order.orderType == "LMT") state.type = OrderType::LIMIT;
    else if (order.orderType == "STP") state.type = OrderType::STOP;
    else if (order.orderType == "STP LMT") state.type = OrderType::STOP_LIMIT;
    else if (order.orderType == "TRAIL") state.type = OrderType::TRAILING_STOP;
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

    // Use ibkrOrderState to access the TWS string status
    state.status = parseIbkrStatus(ibkrOrderState.status);

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

OrderStatus IbkrExecutionEngine::parseIbkrStatus(const std::string& status) {
    if (status == "Submitted" || status == "PreSubmitted") return OrderStatus::NEW;
    if (status == "Filled") return OrderStatus::FILLED;
    if (status == "PartiallyFilled") return OrderStatus::PARTIALLY_FILLED;
    if (status == "Cancelled") return OrderStatus::CANCELED;
    if (status == "Inactive") return OrderStatus::REJECTED; 
    return OrderStatus::NEW;
}

std::optional<double> IbkrExecutionEngine::getAvailableCash() {
    if (cashHasValue_.load()) return cachedCash_.load();
    return std::nullopt;
}

std::optional<double> IbkrExecutionEngine::getPortfolioValue() {
    if (portHasValue_.load()) return cachedPortfolioValue_.load();
    return std::nullopt;
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
        result.push_back(state);
    }
    return result;
}

void IbkrExecutionEngine::onError(int reqId, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) {
    // Error Code 201 = "Order rejected"
    // Error Code 202 = "Order canceled" (Usually triggered by your own cancel request, 
    //                   but sometimes by exchange limits)
    
    if (errorCode == 201 || errorCode == 202) {
        if (!advancedOrderRejectJson.empty()) {
            LOG_WARN("[IbkrExecutionEngine] Order {} Rejected/Canceled: {} | Advanced Info: {}", 
                     reqId, errorString, advancedOrderRejectJson);
        } else {
            LOG_WARN("[IbkrExecutionEngine] Order {} Rejected/Canceled: {}", reqId, errorString);
        }
        
        std::string baseClientId; // Store the already-stripped ID here
        OrderState updatedState;
        bool orderFound = false;

        // 1. Check our ID mapping first (This has the suffix, so we strip it)
        {
            std::lock_guard<std::mutex> lock(idMutex_);
            auto it = orderIdToClientRef_.find(reqId);
            if (it != orderIdToClientRef_.end()) {
                baseClientId = OrderIdUtils::extractBaseClientId(it->second); 
                orderFound = true;
            }
        }

        // 2. Update openOrders_ map if it happens to be tracked there
        {
            std::lock_guard<std::mutex> lock(orderMutex_);
            auto it = openOrders_.find(reqId);
            if (it != openOrders_.end()) {
                it->second.status = (errorCode == 201) ? OrderStatus::REJECTED : OrderStatus::CANCELED;
                updatedState = it->second;
                
                // Fallback: if we only found it here, it is ALREADY stripped by onOpenOrder
                if (!orderFound) {
                    baseClientId = it->second.client_order_id; 
                    orderFound = true;
                }
            }
        }

        // 3. Fire the execution report back to the strategy
        if (orderFound && reportCb_) {
            ExecutionReport report;
            report.order_id = std::to_string(reqId);
            report.client_order_id = baseClientId; // Use the cleanly resolved ID
            
            report.symbol = updatedState.symbol; 
            report.side = updatedState.side;
            report.status = (errorCode == 201) ? OrderStatus::REJECTED : OrderStatus::CANCELED;
            
            report.last_quantity = 0.0;
            report.cumulative_quantity = 0.0; 
            report.last_price = 0.0;
            report.avg_price = 0.0;
            
            report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count();

            reportCb_(report);
        }
    }
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