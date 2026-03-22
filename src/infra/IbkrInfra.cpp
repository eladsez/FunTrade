#include <infra/IbkrInfra.hpp>
#include <core/Logger.hpp>

namespace bot {

IbkrInfra::IbkrInfra(std::string host, int port, int clientId)
    : host_(std::move(host)), port_(port), clientId_(clientId) {
    
    // TWS requires an OS signal to know when network data arrives
    osSignal_ = std::make_unique<EReaderOSSignal>(2000); 
    client_ = std::make_unique<EClientSocket>(this, osSignal_.get());
}

IbkrInfra::~IbkrInfra() {
    disconnectFromTws();
}

bool IbkrInfra::connectToTws() {
    LOG_INFO("[IbkrInfra] Connecting to IB Gateway at {}:{} ClientID: {}", host_, port_, clientId_);
    
    // Using Async connection mode (false parameter = not extraAuth)
    bool connected = client_->eConnect(host_.c_str(), port_, clientId_, false);

    if (connected) {
        LOG_INFO("[IbkrInfra] Socket connected. Starting reader thread...");
        
        // EReader fetches data from the socket and places it in a queue
        reader_ = std::make_unique<EReader>(client_.get(), osSignal_.get());
        reader_->start();

        // Start our background loop to process the queue
        isRunning_ = true;
        readerThread_ = std::thread(&IbkrInfra::readerLoop, this);
        return true;
    } else {
        LOG_ERROR("[IbkrInfra] Failed to connect to IB Gateway.");
        return false;
    }
}

void IbkrInfra::disconnectFromTws() {
    isRunning_ = false;
    if (client_ && client_->isConnected()) {
        client_->eDisconnect();
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
}

void IbkrInfra::readerLoop() {
    LOG_DEBUG("[IbkrInfra] Reader thread started.");
    
    // This loop constantly waits for the OS signal, then tells the reader to process messages,
    // which in turn fires the EWrapper override methods below.
    while (isRunning_ && client_->isConnected()) {
        osSignal_->waitForSignal();
        reader_->processMsgs();
    }
    
    LOG_DEBUG("[IbkrInfra] Reader thread exiting.");
}

// =========================================================================
// EWrapper Callbacks -> Route to std::functions
// =========================================================================

void IbkrInfra::tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib) {
    if (tickPriceCb_) tickPriceCb_(tickerId, field, price, attrib);
}

void IbkrInfra::tickSize(TickerId tickerId, TickType field, Decimal size) {
    if (tickSizeCb_) tickSizeCb_(tickerId, field, size);
}

void IbkrInfra::historicalData(TickerId reqId, const Bar& bar) {
    if (histDataCb_) histDataCb_(reqId, bar);
}

void IbkrInfra::historicalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr) {
    if (histDataEndCb_) histDataEndCb_(reqId, startDateStr, endDateStr);
}

void IbkrInfra::nextValidId(OrderId orderId) {
    if (nextValidIdCb_) nextValidIdCb_(orderId);
}

void IbkrInfra::orderStatus( OrderId orderId, const std::string& status, Decimal filled,
    Decimal remaining, double avgFillPrice, long long permId, int parentId,
    double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice) {
    if (orderStatusCb_) orderStatusCb_(orderId, status, filled, remaining, avgFillPrice, permId, parentId, lastFillPrice, clientId, whyHeld, mktCapPrice);
}

void IbkrInfra::execDetails(int reqId, const Contract& contract, const Execution& execution) {
    if (execDetailsCb_) execDetailsCb_(reqId, contract, execution);
}

// --- System & Error Handling ---

// Add time_t errorTime and cast unused parameters
void IbkrInfra::error(int id, time_t errorTime, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) {
    // Suppress unused parameter warnings
    (void)id;
    (void)errorTime;
    (void)advancedOrderRejectJson;

    // TWS uses the 'error' callback for BOTH errors and benign informational messages
    if (errorCode >= 2100 && errorCode <= 2110) {
        LOG_WARN("[IbkrInfra] TWS Warning ({}): {}", errorCode, errorString);
    } else {
        LOG_ERROR("[IbkrInfra] TWS Error ({}): {}", errorCode, errorString);
    }
}

void IbkrInfra::connectionClosed() {
    LOG_WARN("[IbkrInfra] Connection closed by TWS.");
    isRunning_ = false;
}

void IbkrInfra::connectAck() {
    if (client_->asyncEConnect()) {
        client_->startApi();
        LOG_INFO("[IbkrInfra] Connected & API Started successfully.");
    }
}

} // namespace bot