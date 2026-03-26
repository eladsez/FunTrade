#include <data/IbkrDataFeed.hpp>
#include <core/Logger.hpp>
#include <Decimal.h>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace bot {

IbkrDataFeed::IbkrDataFeed(std::shared_ptr<IbkrInfra> infra, bool useTickByTick) 
    : infra_(std::move(infra)), useTickByTick_(useTickByTick) {}

bool IbkrDataFeed::connectToInfra() {
    LOG_INFO("[IbkrDataFeed] Hooking up tick-by-tick callbacks to Infra...");
    
    infra_->setTickByTickBidAskCallback(
        [this](int reqId, time_t time, double bidPrice, double askPrice, Decimal bidSize, Decimal askSize, const TickAttribBidAsk& tickAttrib) {
            this->onTickByTickBidAsk(reqId, time, bidPrice, askPrice, bidSize, askSize, tickAttrib);
        }
    );

    infra_->setTickByTickAllLastCallback(
        [this](int reqId, int tickType, time_t time, double price, Decimal size, const TickAttribLast& tickAttrib, const std::string& exchange, const std::string& specialConditions) {
            this->onTickByTickAllLast(reqId, tickType, time, price, size, tickAttrib, exchange, specialConditions);
        }
    );

    infra_->setTickByTickMidPointCallback(
        [this](int reqId, time_t time, double midPoint) {
            this->onTickByTickMidPoint(reqId, time, midPoint);
        }
    );

    infra_->setTickPriceCallback([this](TickerId tickerId, TickType field, double price, const TickAttrib& attribs) {
        this->onTickPrice(tickerId, field, price, attribs);
    });

    infra_->setTickSizeCallback([this](TickerId tickerId, TickType field, Decimal size) {
        this->onTickSize(tickerId, field, size);
    });

    infra_->setHistDataCallback(
        [this](TickerId reqId, const Bar& bar) {
            this->onHistoricalData(reqId, bar);
        }
    );

    infra_->setHistDataEndCallback(
        [this](int reqId, const std::string& start, const std::string& end) {
            this->onHistoricalDataEnd(reqId, start, end);
        }
    );

    return true;
}

void IbkrDataFeed::disconnectFromInfra() {
    LOG_INFO("[IbkrDataFeed] Disconnecting DataFeed from Infra.");
}

void IbkrDataFeed::subscribeToSymbol(const std::string& symbol) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) return;

    SymbolSubscriptionIds ids;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (activeSubscriptions_.count(symbol)) return;

        if (useTickByTick_) {
            ids.tbtBidAskId = nextTickerId_++;
            ids.tbtAllLastId = nextTickerId_++;
            ids.tbtMidPointId = nextTickerId_++;
            idToSymbol_[ids.tbtBidAskId] = symbol;
            idToSymbol_[ids.tbtAllLastId] = symbol;
            idToSymbol_[ids.tbtMidPointId] = symbol;
        } else {
            ids.mktDataId = nextTickerId_++;
            idToSymbol_[ids.mktDataId] = symbol;
            mktDataCache_[ids.mktDataId] = TopOfBookCache();
        }
        
        activeSubscriptions_[symbol] = ids;
    }

    Contract contract = createContract(symbol);

    if (useTickByTick_) {
        LOG_INFO("[IbkrDataFeed] Requesting TICK-BY-TICK for {}", symbol);
        infra_->getClient()->reqTickByTickData(ids.tbtBidAskId, contract, "BidAsk", 0, false);
        infra_->getClient()->reqTickByTickData(ids.tbtAllLastId, contract, "AllLast", 0, false);
        infra_->getClient()->reqTickByTickData(ids.tbtMidPointId, contract, "MidPoint", 0, false);
    } else {
        LOG_INFO("[IbkrDataFeed] Requesting MKT DATA (L1) for {}", symbol);
        infra_->getClient()->reqMktData(ids.mktDataId, contract, "", false, false, TagValueListSPtr());
    }
}

void IbkrDataFeed::unsubscribeFromSymbol(const std::string& symbol) {
    SymbolSubscriptionIds ids;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        auto it = activeSubscriptions_.find(symbol);
        if (it == activeSubscriptions_.end()) return;

        ids = it->second;
        
        // Clean maps
        if (useTickByTick_) {
            idToSymbol_.erase(ids.tbtBidAskId);
            idToSymbol_.erase(ids.tbtAllLastId);
            idToSymbol_.erase(ids.tbtMidPointId);
        } else {
            idToSymbol_.erase(ids.mktDataId);
            mktDataCache_.erase(ids.mktDataId);
        }
        activeSubscriptions_.erase(it);
    }

    if (infra_->getClient() && infra_->getClient()->isConnected()) {
        if (useTickByTick_) {
            infra_->getClient()->cancelTickByTickData(ids.tbtBidAskId);
            infra_->getClient()->cancelTickByTickData(ids.tbtAllLastId);
            infra_->getClient()->cancelTickByTickData(ids.tbtMidPointId);
        } else {
            infra_->getClient()->cancelMktData(ids.mktDataId);
        }
    }
}

std::vector<std::string> IbkrDataFeed::getActiveSubscriptions() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<std::string> active;
    for (const auto& pair : activeSubscriptions_) {
        active.push_back(pair.first);
    }
    return active;
}

void IbkrDataFeed::setMarketTickCallback(std::function<void(const MarketEvent&)> callback) {
    tickCallback_ = std::move(callback);
}

// =========================================================================
// The Atomic Callback Processors
// =========================================================================

void IbkrDataFeed::onTickByTickBidAsk(int reqId, time_t time, double bidPrice, double askPrice, Decimal bidSize, Decimal askSize, const TickAttribBidAsk& tickAttrib) {
    (void)tickAttrib; // Can map conditions later if needed
    
    std::string symbol = getSymbolFromId(reqId);
    if (symbol.empty()) return; 

    QuoteTick quote;
    quote.symbol = symbol;
    quote.timestamp_ms = static_cast<uint64_t>(time) * 1000; // True exchange timestamp!
    quote.bid_price = bidPrice;
    quote.ask_price = askPrice;
    quote.bid_size = (bidSize == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(bidSize);
    quote.ask_size = (askSize == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(askSize);

    if (quote.bid_price > 0 && quote.ask_price > 0 && tickCallback_) {
        tickCallback_(quote);
    }
}

void IbkrDataFeed::onTickByTickAllLast(int reqId, int tickType, time_t time, double price, Decimal size, const TickAttribLast& tickAttrib, const std::string& exchange, const std::string& specialConditions) {
    (void)tickType; (void)tickAttrib; (void)exchange; (void)specialConditions;
    
    std::string symbol = getSymbolFromId(reqId);
    if (symbol.empty()) return;

    TradeTick trade;
    trade.symbol = symbol;
    trade.timestamp_ms = static_cast<uint64_t>(time) * 1000; // True exchange timestamp!
    trade.price = price;
    trade.volume = (size == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(size);

    if (trade.volume > 0 && tickCallback_) {
        tickCallback_(trade);
    }
}

void IbkrDataFeed::onTickByTickMidPoint(int reqId, time_t time, double midPoint) {
    std::string symbol = getSymbolFromId(reqId);
    if (symbol.empty()) return;

    MidPointTick tick;
    tick.symbol = symbol;
    tick.timestamp_ms = static_cast<uint64_t>(time) * 1000;
    tick.mid_price = midPoint;

    if (tick.mid_price > 0 && tickCallback_) {
        tickCallback_(tick);
    }
}

void IbkrDataFeed::onTickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& /*attribs*/) {
    std::string symbol = getSymbolFromId(tickerId);
    if (symbol.empty() || price <= 0.0) return;

    // We will construct the events inside the lock, but emit them OUTSIDE the lock
    bool emitTrade = false;
    bool emitMidPoint = false;
    bool emitQuote = false;
    TradeTick trade;
    MidPointTick midTick;
    QuoteTick quoteTick;

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        auto& cache = mktDataCache_[tickerId];
        bool quoteChanged = false;

        if (field == TickType::BID) {
            cache.bid_price = price;
            quoteChanged = true;
        } else if (field == TickType::ASK) {
            cache.ask_price = price;
            quoteChanged = true;
        } else if (field == TickType::LAST) {
            trade.symbol = symbol;
            trade.price = price;
            trade.timestamp_ms = now_ms;
            emitTrade = true;
        }

        if (quoteChanged) {
            // 1. Can we emit a MidPoint?
            if (cache.isValidMidPoint()) {
                midTick.symbol = symbol;
                midTick.mid_price = (cache.bid_price + cache.ask_price) / 2.0;
                midTick.timestamp_ms = now_ms;
                emitMidPoint = true;
            }

            // 2. Can we emit a full Quote?
            if (cache.isValidQuote()) {
                quoteTick.symbol = symbol;
                quoteTick.bid_price = cache.bid_price;
                quoteTick.ask_price = cache.ask_price;
                quoteTick.bid_size = DecimalFunctions::decimalToDouble(cache.bid_size);
                quoteTick.ask_size = DecimalFunctions::decimalToDouble(cache.ask_size);
                quoteTick.timestamp_ms = now_ms;
                emitQuote = true;
            }
        }
    }

    // Safely emit to the eventBus without holding the mutex
    if (emitTrade && tickCallback_) tickCallback_(trade);
    if (emitMidPoint && tickCallback_) tickCallback_(midTick);
    if (emitQuote && tickCallback_) tickCallback_(quoteTick);
}

void IbkrDataFeed::onTickSize(TickerId tickerId, TickType field, Decimal size) {
    std::string symbol = getSymbolFromId(tickerId);
    if (symbol.empty()) return;

    bool emitQuote = false;
    QuoteTick quoteTick;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        auto& cache = mktDataCache_[tickerId];
        bool sizeChanged = false;

        if (field == TickType::BID_SIZE) {
            cache.bid_size = size;
            sizeChanged = true;
        } else if (field == TickType::ASK_SIZE) {
            cache.ask_size = size;
            sizeChanged = true;
        }
        
        // If the size changed, and we have a valid 2-sided market, emit the new Quote state
        if (sizeChanged && cache.isValidQuote()) {
            quoteTick.symbol = symbol;
            quoteTick.bid_price = cache.bid_price;
            quoteTick.ask_price = cache.ask_price;
            quoteTick.bid_size = (cache.bid_size == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(cache.bid_size);
            quoteTick.ask_size = (cache.ask_size == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(cache.ask_size);
            quoteTick.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            emitQuote = true;
        }
    }

    // Safely emit outside the lock
    if (emitQuote && tickCallback_) {
        tickCallback_(quoteTick);
    }

    // Note: We don't recalculate the MidPoint here because the midpoint *price* // hasn't changed, only the size at those levels. 
    // If your strategy ever evolves to use Volume Weighted Midprice (VWM), 
    // you would trigger the calculation here as well!
}

// =========================================================================
// Helpers
// =========================================================================

std::string IbkrDataFeed::getSymbolFromId(int reqId) {
    std::lock_guard<std::mutex> lock(dataMutex_); // Quick lock for map access
    auto it = idToSymbol_.find(reqId);
    if (it != idToSymbol_.end()) {
        return it->second;
    }
    return "";
}

Contract IbkrDataFeed::createContract(const std::string& symbol) {
    Contract contract;

    // Check if the symbol is formatted as a Forex pair (e.g., "EUR.USD")
    auto dotPos = symbol.find('.');
    
    if (dotPos != std::string::npos) {
        // Build Forex Contract
        contract.symbol = symbol.substr(0, dotPos);       // "EUR"
        contract.currency = symbol.substr(dotPos + 1);    // "USD"
        contract.secType = "CASH";
        contract.exchange = "IDEALPRO";
    } else {
        // Build Default US Stock Contract
        contract.symbol = symbol;
        contract.secType = "STK";
        contract.currency = "USD";
        contract.exchange = "SMART";
    }

    return contract;
}

std::vector<HistoricalBar> IbkrDataFeed::fetchHistoricalBars(const std::string& symbol, const std::string& start, const std::string& end, const std::string& timeframe) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) {
        LOG_ERROR("[IbkrDataFeed] Cannot fetch historical data. Not connected to TWS.");
        return {};
    }

    // 1. Prepare formatting (Strategy sends ISO8601, IBKR needs "yyyyMMdd HH:mm:ss")
    std::string ibkrEndDateTime = formatIbkrDate(end);
    std::string durationStr = calculateDurationDays(start, end) + " D";
    std::string barSize = (timeframe == "1Day") ? "1 day" : timeframe; 

    int reqId = nextHistReqId_++;
    std::future<std::vector<HistoricalBar>> future;

    {
        std::lock_guard<std::mutex> lock(histMutex_);
        histPromises_[reqId] = std::promise<std::vector<HistoricalBar>>();
        future = histPromises_[reqId].get_future();
        histDataBuffer_[reqId] = std::vector<HistoricalBar>();
        histIdToSymbol_[reqId] = symbol;
    }

    Contract contract = createContract(symbol);

    LOG_DEBUG("[IbkrDataFeed] Requesting Historical Data for {} (Duration: {}, End: {})", symbol, durationStr, ibkrEndDateTime);
    
    // whatToShow = "TRADES", useRTH = 1, formatDate = 1 (String format)
    infra_->getClient()->reqHistoricalData(reqId, contract, ibkrEndDateTime, durationStr, barSize, "TRADES", 1, 1, false, TagValueListSPtr());

    // 2. Block and wait for the EWrapper callbacks to resolve the promise
    // Use a 30-second timeout to prevent the strategy thread from hanging on a bad connection
    if (future.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
        return future.get();
    } else {
        LOG_ERROR("[IbkrDataFeed] Historical data request timed out for {}", symbol);
        infra_->getClient()->cancelHistoricalData(reqId); // Cleanup in TWS
        
        std::lock_guard<std::mutex> lock(histMutex_);
        histPromises_.erase(reqId);
        histDataBuffer_.erase(reqId);
        histIdToSymbol_.erase(reqId);
        return {};
    }
}

void IbkrDataFeed::onHistoricalData(TickerId reqId, const Bar& bar) {
    std::lock_guard<std::mutex> lock(histMutex_);
    if (histDataBuffer_.find(reqId) != histDataBuffer_.end()) {
        HistoricalBar hBar;
        hBar.symbol = histIdToSymbol_[reqId];
        hBar.timestamp_ms = parseIbkrDateToMs(bar.time); // bar.time is a string because formatDate = 1
        hBar.open = bar.open;
        hBar.high = bar.high;
        hBar.low = bar.low;
        hBar.close = bar.close;
        hBar.volume = (bar.volume == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(bar.volume);
        
        histDataBuffer_[reqId].push_back(hBar);
    }
}

void IbkrDataFeed::onHistoricalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr) {
    (void)startDateStr; (void)endDateStr;
    
    std::lock_guard<std::mutex> lock(histMutex_);
    auto it = histPromises_.find(reqId);
    if (it != histPromises_.end()) {
        // Fulfill the promise, unblocking the strategy thread
        it->second.set_value(histDataBuffer_[reqId]);
        
        // Cleanup memory
        histPromises_.erase(it);
        histDataBuffer_.erase(reqId);
        histIdToSymbol_.erase(reqId);
    }
}

std::string IbkrDataFeed::formatIbkrDate(const std::string& iso8601) {
    // Basic validation for "YYYY-MM-DDTHH:MM:SSZ"
    if (iso8601.length() < 19) return "";
    
    // Extract strictly what we need
    std::string year  = iso8601.substr(0, 4);
    std::string month = iso8601.substr(5, 2);
    std::string day   = iso8601.substr(8, 2);
    std::string time  = iso8601.substr(11, 8);
    
    // Assemble into the explicit IBKR format: "yyyyMMdd HH:mm:ss TMZ"
    return year + month + day + " " + time + " UTC"; 
}

// Approximates the duration string (e.g., "60 D") based on strategy bounds
std::string IbkrDataFeed::calculateDurationDays(const std::string& start, const std::string& end) {
    std::tm tm_start = {}, tm_end = {};
    std::istringstream ss_start(start);
    std::istringstream ss_end(end);
    
    ss_start >> std::get_time(&tm_start, "%Y-%m-%dT%H:%M:%SZ");
    ss_end >> std::get_time(&tm_end, "%Y-%m-%dT%H:%M:%SZ");
    
    auto time_start = std::mktime(&tm_start);
    auto time_end = std::mktime(&tm_end);
    
    // Convert seconds to days, ensure minimum of 1
    int days = std::max(1, static_cast<int>(std::difftime(time_end, time_start) / (60 * 60 * 24)));
    return std::to_string(days);
}

// Converts IBKR's "yyyyMMdd" output string back to a timestamp
uint64_t IbkrDataFeed::parseIbkrDateToMs(const std::string& ibkrDate) {
    if (ibkrDate.length() == 8) { // format for 1 day bars is "yyyyMMdd"
        std::tm tm_val = {};
        std::istringstream ss(ibkrDate);
        ss >> std::get_time(&tm_val, "%Y%m%d");
        return static_cast<uint64_t>(std::mktime(&tm_val)) * 1000;
    }
    return 0;
}

} // namespace bot