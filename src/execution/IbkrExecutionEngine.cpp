#include <execution/IbkrExecutionEngine.hpp>
#include <core/Logger.hpp>

// TWS API Includes
#include <Order.h>
#include <OrderCancel.h> // Required for newer TWS APIs
#include <Contract.h>
#include <Execution.h>
#include <Decimal.h> 

#include <chrono>

namespace bot {

IbkrExecutionEngine::IbkrExecutionEngine(std::shared_ptr<IbkrInfra> infra, IEventBus* eventBus)
    : infra_(std::move(infra)), eventBus_(eventBus) {}

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

    if (infra_->getClient() && infra_->getClient()->isConnected()) {
        infra_->getClient()->reqIds(-1); 
    }

    return true;
}

void IbkrExecutionEngine::disconnectFromInfra() {
    LOG_INFO("[IbkrExecutionEngine] Disconnecting ExecutionEngine from Infra.");
}

// =========================================================================
// Order Management
// =========================================================================

std::string IbkrExecutionEngine::placeOrder(const OrderRequest& request) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) {
        LOG_ERROR("[IbkrExecutionEngine] Cannot place order. Not connected to TWS.");
        return "";
    }

    int ibkrOrderId = 0;
    {
        std::unique_lock<std::mutex> lock(idMutex_);
        idCv_.wait(lock, [this]{ return validIdReceived_; });
        
        ibkrOrderId = nextValidId_++;
        clientRefToOrderId_[request.client_order_id] = ibkrOrderId;
        orderIdToClientRef_[ibkrOrderId] = request.client_order_id;
    }

    Contract contract = createContract(request.symbol);

    ::Order order; 
    order.orderId = ibkrOrderId;
    order.orderRef = request.client_order_id; 
    
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

    LOG_INFO("[IbkrExecutionEngine] Placing {} Order {} for {} {} (ID: {}, Ref: {})", 
             order.orderType, order.action, DecimalFunctions::decimalToDouble(order.totalQuantity), 
             request.symbol, ibkrOrderId, request.client_order_id);

    infra_->getClient()->placeOrder(ibkrOrderId, contract, order);
    return request.client_order_id;
}

bool IbkrExecutionEngine::cancelOrder(const std::string& client_order_id) {
    std::lock_guard<std::mutex> lock(idMutex_);
    auto it = clientRefToOrderId_.find(client_order_id);
    
    if (it != clientRefToOrderId_.end()) {
        int orderId = it->second;
        LOG_INFO("[IbkrExecutionEngine] Cancelling order Ref: {} (ID: {})", client_order_id, orderId);
        
        ::OrderCancel cancelReq; // New TWS API requires this struct
        infra_->getClient()->cancelOrder(orderId, cancelReq); 
        return true;
    } else {
        LOG_WARN("[IbkrExecutionEngine] Could not find IBKR ID for cancel request on Ref: {}", client_order_id);
        return false;
    }
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
    report.client_order_id = clientRef;
    report.status = parseIbkrStatus(status);
    
    report.cumulative_quantity = (filled == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(filled);
    report.avg_price = avgFillPrice;
    report.last_price = lastFillPrice; 
    report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();

    if (eventBus_) {
        eventBus_->publishExecution(report);
    }
}

void IbkrExecutionEngine::onExecDetails(int reqId, const Contract& contract, const Execution& execution) {
    (void)reqId; 

    ExecutionReport report;
    report.order_id = std::to_string(execution.orderId);
    report.client_order_id = execution.orderRef; 
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

    if (eventBus_) {
        eventBus_->publishExecution(report);
    }
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
    return cachedCash_.load();
}

std::optional<double> IbkrExecutionEngine::getPortfolioValue() {
    return cachedPortfolioValue_.load();
}

nlohmann::json IbkrExecutionEngine::getOpenPositions() {
    return nlohmann::json::array();
}

nlohmann::json IbkrExecutionEngine::getOpenOrders() {
    return nlohmann::json::array();
}

} // namespace bot