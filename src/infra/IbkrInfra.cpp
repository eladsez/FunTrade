#include <infra/IbkrInfra.hpp>
#include <core/Logger.hpp>

#include <chrono>

namespace bot {

namespace {
int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

void IbkrInfra::log(const std::optional<ConnectionHealth::Notice>& n) {
    if (!n) return;
    switch (n->level) {
        case ConnectionHealth::Level::Debug: LOG_DEBUG("[IbkrInfra] {}", n->text); break;
        case ConnectionHealth::Level::Info:  LOG_INFO("[IbkrInfra] {}", n->text); break;
        case ConnectionHealth::Level::Warn:  LOG_WARN("[IbkrInfra] {}", n->text); break;
        case ConnectionHealth::Level::Error: LOG_ERROR("[IbkrInfra] {}", n->text); break;
    }
}

void IbkrInfra::checkHealth() { log(health_.tick(nowMs())); }

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
    client_.reset();
    osSignal_.reset();
}

void IbkrInfra::cleanupConnection() {
    isRunning_ = false;
    isConnected_ = false;

    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex_);
        if (client_ && client_->isConnected()) {
            client_->eDisconnect();
        }
    }
    
    if (osSignal_) {
        osSignal_->issueSignal(); // Unblock the reader thread safely
    }

    // Join WITHOUT the client lock: a callback on the reader thread may be waiting for it.
    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    reader_.reset();
    // client_ and osSignal_ are kept: other threads may hold the client pointer, and
    // EClientSocket supports eConnect() again after eDisconnect().
}

bool IbkrInfra::tryConnect() {
    LOG_INFO("[IbkrInfra] Attempting connection to IB Gateway {}:{}", host_, port_);
    cleanupConnection();

    std::lock_guard<std::recursive_mutex> lock(clientMutex_);
    if (client_->eConnect(host_.c_str(), port_, clientId_, false)) {
        LOG_DEBUG("[IbkrInfra] Socket connected. Starting reader thread...");
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
            // Wait for isConnected_ to become true (set by nextValidId)
            std::unique_lock<std::mutex> connectLock(connectMutex_);
            connectCv_.wait_for(connectLock, std::chrono::seconds(10), [this](){ 
                return isConnected_.load() || stopRequested_.load(); 
            });
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
        log(health_.socketUp(nowMs()));
        connectCv_.notify_all(); // Unblock the constructor or the monitor thread
        // Check if this is a reconnect
        if (!isInitialConnection_.load()) {
            LOG_INFO("[IbkrInfra] Reconnection detected. Firing reconnect callbacks...");
            fireReconnectCallbacks();
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

void IbkrInfra::execDetailsEnd(int reqId) {
    if (execDetailsEndCb_) execDetailsEndCb_(reqId);
}

void IbkrInfra::fireReconnectCallbacks() {
    for (const auto& cb : reconnectCallbacks_) {
        if (cb) cb();
    }
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

void IbkrInfra::contractDetails(int reqId, const ContractDetails& contractDetails) {
    if (contractDetailsCb_) contractDetailsCb_(reqId, contractDetails);
}

void IbkrInfra::contractDetailsEnd(int reqId) {
    if (contractDetailsEndCb_) contractDetailsEndCb_(reqId);
}

// --- System & Error Handling ---

void IbkrInfra::error(int id, time_t errorTime, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) {
    (void)errorTime; (void)advancedOrderRejectJson;
    // 21xx are status notices (data farm OK, ...); 10167 is "showing delayed data", expected
    // without a real-time subscription. Anything else (e.g. 10089/354/162 missing market data
    // permissions, order rejections) must be visible in the log.
    bool notice = (errorCode >= 2100 && errorCode < 2200) || errorCode == 10167 ||
                  (errorCode >= 1100 && errorCode <= 1102);  // logged below
    if (notice) LOG_DEBUG("[IbkrInfra] TWS ({}): {}", errorCode, errorString);
    else LOG_WARN("[IbkrInfra] TWS error {} (req {}): {}", errorCode, id, errorString);

    // Socket-level loss (our connection to TWS/Gateway): reconnect.
    if (errorCode == 504 || errorCode == 502) { 
        LOG_WARN("[IbkrInfra] Connection loss detected (Code {}). Watchdog taking over.", errorCode);
        handleDisconnect();
    }
    // TWS <-> IB servers. Our socket stays up, so the watchdog never sees these.
    else if (errorCode == 1100) {
        log(health_.ibkrDown(nowMs()));
    } else if (errorCode == 1101) {
        log(health_.ibkrUp(nowMs(), true));
        fireReconnectCallbacks();
    } else if (errorCode == 1102) {
        log(health_.ibkrUp(nowMs(), false));
    }

    if (errorCb_) errorCb_(id, errorCode, errorString, advancedOrderRejectJson);
}

void IbkrInfra::currentTime(long time) {
    if (currentTimeCb_) currentTimeCb_(time);
}

void IbkrInfra::connectionClosed() {
    LOG_WARN("[IbkrInfra] Connection closed by TWS. Watchdog will attempt reconnect.");
    handleDisconnect();
}

void IbkrInfra::handleDisconnect() {
    isRunning_ = false; // Stop the reader loop immediately
    log(health_.socketDown(nowMs()));

    if (isConnected_.exchange(false)) {
        // We were fully connected. Wake up the main monitor wait.
        monitorCv_.notify_one(); 
    } else {
        // We were in the middle of a handshake! Interrupt the 10-second wait.
        connectCv_.notify_all(); 
    }
}

void IbkrInfra::connectAck() {
    if (client_->asyncEConnect()) {
        client_->startApi();
        LOG_DEBUG("[IbkrInfra] Socket connected. Waiting for NextValidId...");
    }
}

} // namespace bot