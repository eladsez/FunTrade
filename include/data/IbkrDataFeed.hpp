#pragma once

#include <data/IDataFeed.hpp>
#include <infra/IbkrInfra.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <future>
#include <chrono>
#include <Decimal.h>
#include <thread>
#include <condition_variable>

namespace bot {

// Tracks the specific reqIds used for a symbol depending on the mode
struct SymbolSubscriptionIds {
    int mktDataId = -1;
    int tbtBidAskId = -1;
    int tbtAllLastId = -1;
    int tbtMidPointId = -1;
};

struct TopOfBookCache {
    double bid_price = 0.0;
    double ask_price = 0.0;
    Decimal bid_size = UNSET_DECIMAL; // Default to UNSET
    Decimal ask_size = UNSET_DECIMAL; // Default to UNSET
    
    // For Mean Reversion: We only need prices to calculate the mid
    bool isValidMidPoint() const {
        return bid_price > 0.0 && ask_price > 0.0 && bid_price < ask_price;
    }

    // For full Order Book / Liquidity strategies: We need prices AND valid sizes
    bool isValidQuote() const {
        if (!isValidMidPoint()) return false;
        if (bid_size == UNSET_DECIMAL || ask_size == UNSET_DECIMAL) return false;
        
        return DecimalFunctions::decimalToDouble(bid_size) > 0.0 && 
            DecimalFunctions::decimalToDouble(ask_size) > 0.0;
    }
};

struct DailyMarketHours {
    bool isClosedAllDay = false;
    uint64_t rth_start_ms = 0;
    uint64_t rth_end_ms = 0;
    std::string liquidHoursStr; // Cache the full string to re-parse if the day rolls over
    std::string timeZoneId;     // e.g., "EST5EDT"
};

class IbkrDataFeed : public IDataFeed {
public:
    IbkrDataFeed(std::shared_ptr<IbkrInfra> infra, bool useTickByTick = false);
    ~IbkrDataFeed() override;

    bool connectToInfra() override;
    void disconnectFromInfra() override;

    void subscribeToSymbol(const std::string& symbol) override;
    void unsubscribeFromSymbol(const std::string& symbol) override;
    std::vector<std::string> getActiveSubscriptions() const override;

    std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, const std::string& start, 
        const std::string& end, const std::string& timeframe) override;

    void setMarketTickCallback(std::function<void(const MarketEvent&)> callback) override;

private:
    // Tick-By-Tick Callbacks
    void onTickByTickBidAsk(int reqId, time_t time, double bidPrice, double askPrice, Decimal bidSize, Decimal askSize, const TickAttribBidAsk& tickAttrib);
    void onTickByTickAllLast(int reqId, int tickType, time_t time, double price, Decimal size, const TickAttribLast& tickAttrib, const std::string& exchange, const std::string& specialConditions);
    void onTickByTickMidPoint(int reqId, time_t time, double midPoint);

    // Top-Of-Book (L1) Callbacks
    void onTickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attribs);
    void onTickSize(TickerId tickerId, TickType field, Decimal size);

    void onHistoricalData(TickerId reqId, const Bar& bar);
    void onHistoricalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr);

    // Market hours Callbacks
    void onContractDetails(int reqId, const ContractDetails& contractDetails);
    void onContractDetailsEnd(int reqId);

    std::string formatIbkrDate(const std::string& iso8601);
    std::string calculateDurationDays(const std::string& start, const std::string& end);
    uint64_t parseIbkrDateToMs(const std::string& ibkrDate);
    std::string getSymbolFromId(int reqId);
    Contract createContract(const std::string& symbol);

    bool isWithinRTH(const std::string& symbol, uint64_t tick_timestamp_ms);
    void updateDailyHoursLimits(DailyMarketHours& hoursInfo, uint64_t current_ts_ms);

    void eodTaskLoop();
    void processEodBars();
    long calcSecondsUntilNextEod(long current_ibkr_utc_time);

    void onReconnect();

    std::shared_ptr<IbkrInfra> infra_;
    
    std::function<void(const MarketEvent&)> tickCallback_;

    mutable std::mutex dataMutex_;
    std::mutex histMutex_;

    // ID Management
    std::atomic<int> nextTickerId_{2000}; 
    std::atomic<int> nextHistReqId_{4000}; // Separate ID space for historical requests
    bool useTickByTick_;
    
    std::unordered_map<std::string, SymbolSubscriptionIds> activeSubscriptions_;
    std::unordered_map<int, TopOfBookCache> mktDataCache_;
    std::unordered_map<int, std::string> idToSymbol_; // Maps any ID back to its symbol
    std::unordered_map<int, std::promise<std::vector<HistoricalBar>>> histPromises_;
    std::unordered_map<int, std::vector<HistoricalBar>> histDataBuffer_;
    std::unordered_map<int, std::string> histIdToSymbol_;

    // eod bars
    std::thread eodThread_;
    std::mutex eodMutex_;
    std::condition_variable eodCv_;
    std::atomic<bool> stopEodThread_{false};
    std::atomic<long> ibkrCurrentTime_{0};
    std::unordered_map<std::string, uint64_t> lastEodTimestamp_;

    // State for Contract Details
    std::atomic<int> nextHoursReqId_{6000}; 
    std::unordered_map<int, std::string> contractReqToSymbol_; 
    std::unordered_map<std::string, DailyMarketHours> symbolMarketHours_;
};

} // namespace bot