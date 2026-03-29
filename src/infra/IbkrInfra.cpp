#include <infra/IbkrInfra.hpp>
#include <core/Logger.hpp>

namespace bot {

IbkrInfra::IbkrInfra(std::string host, int port, int clientId)
    : host_(std::move(host)), port_(port), clientId_(clientId) {
    
    // TWS requires an OS signal to know when network data arrives
    osSignal_ = std::make_unique<EReaderOSSignal>(2000); 
    client_ = std::make_unique<EClientSocket>(this, osSignal_.get());

    monitorThread_ = std::thread(&IbkrInfra::monitorLoop, this);

    LOG_INFO("[IbkrInfra] Waiting for TWS connection and initialization...");
    std::unique_lock<std::mutex> lock(connectMutex_);
    if (connectCv_.wait_for(lock, std::chrono::seconds(10), [this]() { return isConnected_.load(); })) {
        LOG_INFO("[IbkrInfra] Successfully initialized and ready.");
    } else {
        LOG_ERROR("[IbkrInfra] Timeout waiting for initial connection. Watchdog will keep trying.");
    }
}

IbkrInfra::~IbkrInfra() {
    stopRequested_ = true;
    monitorCv_.notify_all();

    if (monitorThread_.joinable()) 
        monitorThread_.join();

    cleanupConnection();
}

void IbkrInfra::cleanupConnection() {
    isRunning_ = false;
    isConnected_ = false;

    if (client_ && client_->isConnected()) {
        client_->eDisconnect();
    }
    
    if (osSignal_) {
        osSignal_->issueSignal(); // Unblock the reader thread safely
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }
}

bool IbkrInfra::tryConnect() {
    LOG_INFO("[IbkrInfra] Attempting connection to IB Gateway {}:{}", host_, port_);
    cleanupConnection();

    if (client_->eConnect(host_.c_str(), port_, clientId_, false)) {
        LOG_INFO("[IbkrInfra] Socket connected. Starting reader thread...");
        reader_ = std::make_unique<EReader>(client_.get(), osSignal_.get());
        reader_->start();

        isRunning_ = true;
        readerThread_ = std::thread(&IbkrInfra::readerLoop, this);
        return true;
    }
    return false;
}

void IbkrInfra::monitorLoop() {
    while (!stopRequested_) {
        std::unique_lock<std::mutex> lock(monitorMutex_);

        // If we are connected, sleep peacefully until a disconnect or stop is requested
        if (isConnected_) {
            monitorCv_.wait(lock, [this]() {
                return !isConnected_.load() || stopRequested_.load();
            });
        }

        if (stopRequested_) break;

        // We are disconnected. Release the lock before attempting network IO
        lock.unlock();

        if (tryConnect()) {
            // Successfully connected. The next loop iteration will block on the CV.
            continue; 
        } else {
            LOG_ERROR("[IbkrInfra] Connection failed. Retrying in 5 seconds...");
            
            // Re-acquire lock to wait. This allows the 5-second retry delay 
            // to be instantly interrupted if the application is shutting down.
            lock.lock();
            monitorCv_.wait_for(lock, std::chrono::seconds(5), [this]() {
                return stopRequested_.load();
            });
        }
    }
}

void IbkrInfra::readerLoop() {
    // This loop constantly waits for the OS signal, then tells the reader to process messages,
    // which in turn fires the EWrapper override methods below.
    while (isRunning_ && client_->isConnected()) {
        osSignal_->waitForSignal();
        if (isRunning_) { 
            reader_->processMsgs();
        }
    }
}

// =========================================================================
// EWrapper Callbacks -> Route to std::functions
// =========================================================================

void IbkrInfra::tickByTickBidAsk(int reqId, time_t time, double bidPrice, double askPrice, Decimal bidSize, Decimal askSize, const TickAttribBidAsk& tickAttrib) {
    if (tickByTickBidAskCb_) tickByTickBidAskCb_(reqId, time, bidPrice, askPrice, bidSize, askSize, tickAttrib);
}

void IbkrInfra::tickByTickAllLast(int reqId, int tickType, time_t time, double price, Decimal size, const TickAttribLast& tickAttrib, const std::string& exchange, const std::string& specialConditions) {
    if (tickByTickAllLastCb_) tickByTickAllLastCb_(reqId, tickType, time, price, size, tickAttrib, exchange, specialConditions);
}

void IbkrInfra::tickByTickMidPoint(int reqId, time_t time, double midPoint) {
    if (tickByTickMidPointCb_) tickByTickMidPointCb_(reqId, time, midPoint);
}

void IbkrInfra::tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attribs) {
    if (tickPriceCb_) tickPriceCb_(tickerId, field, price, attribs);
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
    if (!isConnected_.load()) {
        LOG_DEBUG("[IbkrInfra] Received Next Valid ID: {}. Connection fully established.", orderId);
        isConnected_ = true;
        connectCv_.notify_all(); // Unblock the constructor!
        // Check if this is a reconnect
        if (!isInitialConnection_.load()) {
            LOG_DEBUG("[IbkrInfra] Reconnection detected. Firing reconnect callbacks...");
            for (const auto& cb : reconnectCallbacks_) {
                if (cb) cb();
            }
        }
        isInitialConnection_ = false;
    }

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

void IbkrInfra::updateAccountValue(const std::string& key, const std::string& val, const std::string& currency, const std::string& accountName) {
    if (updateAccountValueCb_) updateAccountValueCb_(key, val, currency, accountName);
}

void IbkrInfra::position(const std::string& account, const Contract& contract, Decimal position, double avgCost) {
    if (positionCb_) positionCb_(account, contract, position, avgCost);
}

void IbkrInfra::openOrder(OrderId orderId, const Contract& contract, const ::Order& order, const ::OrderState& orderState) {
    if (openOrderCb_) openOrderCb_(orderId, contract, order, orderState);
}

void IbkrInfra::accountDownloadEnd(const std::string& accountName) {
    if (accountDownloadEndCb_) accountDownloadEndCb_(accountName);
}

void IbkrInfra::positionEnd() {
    if (positionEndCb_) positionEndCb_();
}

void IbkrInfra::openOrderEnd() {
    if (openOrderEndCb_) openOrderEndCb_();
}

// --- System & Error Handling ---

void IbkrInfra::error(int id, time_t errorTime, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) {
    (void)id; (void)errorTime; (void)advancedOrderRejectJson;
    LOG_DEBUG("[IbkrInfra] TWS ({}): {}", errorCode, errorString);

    // Common disconnect codes: 502, 504, 1100, 2110
    if (errorCode == 504 || errorCode == 502 || errorCode == 1100 || errorCode == 2110) { 
        LOG_WARN("[IbkrInfra] Connection loss detected (Code {}). Watchdog taking over.", errorCode);
        isConnected_ = false; 
        isRunning_ = false;
        monitorCv_.notify_one(); // Wake up the monitor loop instantly
    }

    if (errorCb_) errorCb_(id, errorCode, errorString, advancedOrderRejectJson);
}

void IbkrInfra::currentTime(long time) {
    if (currentTimeCb_) currentTimeCb_(time);
}

void IbkrInfra::connectionClosed() {
    LOG_WARN("[IbkrInfra] Connection closed by TWS. Watchdog will attempt reconnect.");
    isConnected_ = false;
    isRunning_ = false;
    monitorCv_.notify_one(); // Wake up the monitor loop instantly
}

void IbkrInfra::connectAck() {
    if (client_->asyncEConnect()) {
        client_->startApi();
        LOG_INFO("[IbkrInfra] Socket connected. Waiting for NextValidId...");
    }
}

} // namespace bot