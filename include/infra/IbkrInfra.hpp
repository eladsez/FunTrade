#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

// TWS API Includes
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReaderOSSignal.h"
#include "EReader.h"

namespace bot {

class IbkrInfra : public DefaultEWrapper {
public:
    IbkrInfra(std::string host, int port, int clientId);
    ~IbkrInfra() override;

    bool connectToTws();
    void disconnectFromTws();
    
    // Accessor so the Feeds/Engines can send commands (reqMktData, placeOrder, etc.)
    EClientSocket* getClient() const { return client_.get(); }

    // =========================================================================
    // Callback Registrations (Used by DataFeed and ExecutionEngine)
    // =========================================================================

    // --- Market Data Callbacks ---
    using TickPriceCallback = std::function<void(TickerId, TickType, double, const TickAttrib&)>;
    using TickSizeCallback = std::function<void(TickerId, TickType, Decimal)>;
    using HistDataCallback = std::function<void(TickerId, const Bar&)>;
    using HistDataEndCallback = std::function<void(int, const std::string&, const std::string&)>;

    void setTickPriceCallback(TickPriceCallback cb) { tickPriceCb_ = std::move(cb); }
    void setTickSizeCallback(TickSizeCallback cb) { tickSizeCb_ = std::move(cb); }
    void setHistDataCallback(HistDataCallback cb) { histDataCb_ = std::move(cb); }
    void setHistDataEndCallback(HistDataEndCallback cb) { histDataEndCb_ = std::move(cb); }

    // --- Execution & Account Callbacks ---
    using NextValidIdCallback = std::function<void(int)>;
    using OrderStatusCallback = std::function<void(OrderId, const std::string&, Decimal, Decimal, double, long long, int, double, int, const std::string&, double)>;
    using ExecDetailsCallback = std::function<void(int, const Contract&, const Execution&)>;
    
    void setNextValidIdCallback(NextValidIdCallback cb) { nextValidIdCb_ = std::move(cb); }
    void setOrderStatusCallback(OrderStatusCallback cb) { orderStatusCb_ = std::move(cb); }
    void setExecDetailsCallback(ExecDetailsCallback cb) { execDetailsCb_ = std::move(cb); }

    // --- Account & Portfolio Callbacks ---
    using UpdateAccountValueCallback = std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)>;
    using PositionCallback = std::function<void(const std::string&, const Contract&, Decimal, double)>;
    using OpenOrderCallback = std::function<void(OrderId, const Contract&, const ::Order&, const ::OrderState&)>;

    void setUpdateAccountValueCallback(UpdateAccountValueCallback cb) { updateAccountValueCb_ = std::move(cb); }
    void setPositionCallback(PositionCallback cb) { positionCb_ = std::move(cb); }
    void setOpenOrderCallback(OpenOrderCallback cb) { openOrderCb_ = std::move(cb); }

    // =========================================================================
    // EWrapper Overrides (Triggered by TWS API background thread)
    // =========================================================================
    
    void tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib) override;
    void tickSize(TickerId tickerId, TickType field, Decimal size) override;
    void historicalData(TickerId reqId, const Bar& bar) override;
    void historicalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr) override;

    void nextValidId(OrderId orderId) override;
    void orderStatus(OrderId orderId, const std::string& status, Decimal filled,
	                Decimal remaining, double avgFillPrice, long long permId, int parentId,
	                double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice) override;
    void execDetails(int reqId, const Contract& contract, const Execution& execution) override;

    void updateAccountValue(const std::string& key, const std::string& val, const std::string& currency, const std::string& accountName) override;
    void position(const std::string& account, const Contract& contract, Decimal position, double avgCost) override;
    void openOrder(OrderId orderId, const Contract& contract, const ::Order& order, const ::OrderState& orderState) override;

    void error(int id, time_t errorTime, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) override;
    void connectionClosed() override;
    void connectAck() override;

private:
    void readerLoop(); // The background thread processing TWS messages

    std::string host_;
    int port_;
    int clientId_;

    std::unique_ptr<EReaderOSSignal> osSignal_;
    std::unique_ptr<EClientSocket> client_;
    std::unique_ptr<EReader> reader_;
    
    std::thread readerThread_;
    std::atomic<bool> isRunning_{false};

    // Callback instances
    TickPriceCallback tickPriceCb_;
    TickSizeCallback tickSizeCb_;
    HistDataCallback histDataCb_;
    HistDataEndCallback histDataEndCb_;
    
    NextValidIdCallback nextValidIdCb_;
    OrderStatusCallback orderStatusCb_;
    ExecDetailsCallback execDetailsCb_;

    UpdateAccountValueCallback updateAccountValueCb_;
    PositionCallback positionCb_;
    OpenOrderCallback openOrderCb_;

};

} // namespace bot