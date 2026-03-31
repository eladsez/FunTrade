#include <data/IbkrDataFeed.hpp>
#include <core/Logger.hpp>
#include <utils/TimeUtils.hpp>
#include <Decimal.h>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <format>

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

    infra_->setCurrentTimeCallback([this](long time) {
        ibkrCurrentTime_.store(time);
        eodCv_.notify_one(); // Wake up the thread if it's waiting for the initial sync
    });

    infra_->setContractDetailsCallback([this](int reqId, const ContractDetails& details) {
        this->onContractDetails(reqId, details);
    });

    infra_->setContractDetailsEndCallback([this](int reqId) {
        this->onContractDetailsEnd(reqId);
    });

    infra_->addReconnectCallback([this]() {
        this->onReconnect(); 
    });

    stopEodThread_ = false;
    eodThread_ = std::thread(&IbkrDataFeed::eodTaskLoop, this);

    return true;
}

void IbkrDataFeed::disconnectFromInfra() {
    // 1. Lock the mutex to ensure the thread isn't evaluating the condition
    {
        std::lock_guard<std::mutex> lock(eodMutex_);
        stopEodThread_ = true;
    }
    
    // 2. Wake up the sleeping thread
    eodCv_.notify_all();
    
    // 3. Wait for it to cleanly exit its loop
    if (eodThread_.joinable()) {
        eodThread_.join();
    }

    LOG_INFO("[IbkrDataFeed] Disconnecting DataFeed from Infra.");
}

IbkrDataFeed::~IbkrDataFeed() {
    disconnectFromInfra();
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

        contractReqToSymbol_[nextHoursReqId_] = symbol;
        activeSubscriptions_[symbol] = ids;
    }

    Contract contract = createContract(symbol);

    LOG_DEBUG("[IbkrDataFeed] Requesting Contract Details for hours: {}", symbol);
    infra_->getClient()->reqContractDetails(nextHoursReqId_++, contract);

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
        symbolMarketHours_.erase(symbol);
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

void IbkrDataFeed::onReconnect() {
    LOG_INFO("[IbkrDataFeed] Re-establishing active subscriptions after reconnect...");
    
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) return;

    for (const auto& [symbol, ids] : activeSubscriptions_) {
        Contract contract = createContract(symbol);
        
        if (useTickByTick_) {
            LOG_DEBUG("[IbkrDataFeed] Re-requesting TICK-BY-TICK for {}", symbol);
            infra_->getClient()->reqTickByTickData(ids.tbtBidAskId, contract, "BidAsk", 0, false);
            infra_->getClient()->reqTickByTickData(ids.tbtAllLastId, contract, "AllLast", 0, false);
            infra_->getClient()->reqTickByTickData(ids.tbtMidPointId, contract, "MidPoint", 0, false);
        } else {
            LOG_DEBUG("[IbkrDataFeed] Re-requesting MKT DATA (L1) for {}", symbol);
            infra_->getClient()->reqMktData(ids.mktDataId, contract, "", false, false, TagValueListSPtr());
        }
    }
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

    uint64_t ts_ms = static_cast<uint64_t>(time) * 1000;

    if (!isWithinRTH(symbol, ts_ms)) {
        return; // Drop pre/post market tick
    }

    QuoteTick quote;
    quote.symbol = symbol;
    quote.timestamp_ms = ts_ms;
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

    uint64_t ts_ms = static_cast<uint64_t>(time) * 1000;

    if (!isWithinRTH(symbol, ts_ms)) {
        return; // Drop pre/post market tick
    }

    TradeTick trade;
    trade.symbol = symbol;
    trade.timestamp_ms = ts_ms;
    trade.price = price;
    trade.volume = (size == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(size);

    if (trade.volume > 0 && tickCallback_) {
        tickCallback_(trade);
    }
}

void IbkrDataFeed::onTickByTickMidPoint(int reqId, time_t time, double midPoint) {
    std::string symbol = getSymbolFromId(reqId);
    if (symbol.empty()) return;

    uint64_t ts_ms = static_cast<uint64_t>(time) * 1000;

    if (!isWithinRTH(symbol, ts_ms)) {
        return; // Drop pre/post market tick
    }

    MidPointTick tick;
    tick.symbol = symbol;
    tick.timestamp_ms = ts_ms;
    tick.mid_price = midPoint;

    if (tick.mid_price > 0 && tickCallback_) {
        tickCallback_(tick);
    }
}

void IbkrDataFeed::onTickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& /*attribs*/) {
    std::string symbol = getSymbolFromId(tickerId);
    if (symbol.empty() || price <= 0.0) return;

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

    if (!isWithinRTH(symbol, now_ms)) {
        return; // Drop pre/post market tick
    }


    // We will construct the events inside the lock, but emit them OUTSIDE the lock
    bool emitTrade = false;
    bool emitMidPoint = false;
    bool emitQuote = false;
    TradeTick trade;
    MidPointTick midTick;
    QuoteTick quoteTick;

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

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

    if (!isWithinRTH(symbol, now_ms)) {
        return; // Drop pre/post market tick
    }

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

void IbkrDataFeed::onContractDetails(int reqId, const ContractDetails& details) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto it = contractReqToSymbol_.find(reqId);
    if (it != contractReqToSymbol_.end()) {
        std::string symbol = it->second;
        DailyMarketHours hours;
        hours.liquidHoursStr = details.liquidHours;
        hours.timeZoneId = details.timeZoneId;
        symbolMarketHours_[symbol] = hours;
        LOG_DEBUG("[IbkrDataFeed] Received Market Hours schedule for {}", symbol);
    }
}

void IbkrDataFeed::onContractDetailsEnd(int reqId) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    contractReqToSymbol_.erase(reqId);
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

void IbkrDataFeed::eodTaskLoop() {
    LOG_INFO("[IbkrDataFeed] EOD Timer thread started.");

    while (!stopEodThread_) {
        // 1. Request the official time from IBKR
        ibkrCurrentTime_.store(0);
        if (infra_->getClient() && infra_->getClient()->isConnected()) {
            infra_->getClient()->reqCurrentTime();
        }

        // 2. Wait until the callback populates the time (with a small timeout for safety)
        std::unique_lock<std::mutex> lock(eodMutex_);
        eodCv_.wait_for(lock, std::chrono::seconds(5), [this] { 
            return ibkrCurrentTime_.load() > 0 || stopEodThread_; 
        });

        if (stopEodThread_) break;

        long current_utc = ibkrCurrentTime_.load();
        if (current_utc == 0) {
            // If IBKR didn't respond, sleep for a minute and retry
            eodCv_.wait_for(lock, std::chrono::minutes(1));
            continue;
        }

        // 3. Calculate seconds to sleep
        long seconds_to_sleep = calcSecondsUntilNextEod(current_utc);
        LOG_DEBUG("[IbkrDataFeed] Synchronized with exchange clock. Sleeping for {} seconds until EOD fetch.", seconds_to_sleep);

        // 4. Sleep using the monotonic clock condition variable
        bool interrupted = eodCv_.wait_for(lock, std::chrono::seconds(seconds_to_sleep), [this] {
            return stopEodThread_.load();
        });

        if (interrupted) break; // System is shutting down

        // 5. Wake up! The market has closed. Fetch the data.
        processEodBars();
    }

    LOG_INFO("[IbkrDataFeed] EOD Timer thread stopped.");
}

void IbkrDataFeed::processEodBars() {
    auto active_subs = getActiveSubscriptions();
    LOG_DEBUG("[IbkrDataFeed] Firing EOD Historical Fetch for {} symbols...", active_subs.size());

    std::string end_time_str = utils::formatTimePoint(std::chrono::system_clock::now());

    for (const auto& sym : active_subs) {
        // Request "1 Day" duration ending today to get the official EOD close
        auto bars = fetchHistoricalBars(sym, end_time_str, end_time_str, "1Day");
        
        if (!bars.empty() && tickCallback_) {
            uint64_t current_bar_ts = bars.back().timestamp_ms;

            // --- Dynamic Holiday Detection ---
            // If the bar's timestamp is equal to or older than the last one we processed,
            // the market was closed today, and IBKR just handed us yesterday's bar again.
            if (lastEodTimestamp_.count(sym) && current_bar_ts <= lastEodTimestamp_[sym]) {
                LOG_DEBUG("[IbkrDataFeed] Skipped EOD bar for {}. Market was likely closed today (Holiday).", sym);
                continue;
            }

            // Update the cache with the new, valid timestamp
            lastEodTimestamp_[sym] = current_bar_ts;

            BarUpdate bar_event;
            bar_event.symbol = sym;
            bar_event.open = bars.back().open;
            bar_event.high = bars.back().high;
            bar_event.low = bars.back().low;
            bar_event.close = bars.back().close;
            bar_event.volume = bars.back().volume;
            bar_event.timestamp_ms = current_bar_ts;

            LOG_DEBUG("[IbkrDataFeed] EOD Bar Retrieved for {}: Close = {:.2f}", sym, bar_event.close);
            
            tickCallback_(bar_event);
        }
    }
}

long IbkrDataFeed::calcSecondsUntilNextEod(long current_ibkr_utc_time) {
    using namespace std::chrono;
    
    // Create a time_point from the IBKR UTC timestamp
    system_clock::time_point current_utc{seconds{current_ibkr_utc_time}};
    
    // Bind it to the New York timezone
    auto ny_zone = locate_zone("America/New_York");
    auto ny_local = ny_zone->to_local(current_utc);
    
    // Extract the calendar date in NY
    auto ny_day = floor<days>(ny_local);
    year_month_day ymd{ny_day};
    
    // Construct 18:00:00 on that specific NY day
    auto target_local = local_days{ymd} + hours{18};
    
    // Convert back to UTC to get the absolute system epoch
    auto target_utc = ny_zone->to_sys(target_local);
    
    // If it's already past 18:00 today, aim for tomorrow 18:00
    if (target_utc <= current_utc) {
        target_utc += hours{24};
        ny_day += days{1}; // Keep track of the target day
    }
    
    // --- Dynamic Weekend Skipping ---
    weekday wd{ny_day};
    if (wd == Saturday) {
        target_utc += hours{48}; // Push Saturday 18:00 to Monday 18:00
    } else if (wd == Sunday) {
        target_utc += hours{24}; // Push Sunday 18:00 to Monday 18:00
    }
    
    return duration_cast<seconds>(target_utc - current_utc).count();
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

void IbkrDataFeed::updateDailyHoursLimits(const std::string& symbol, DailyMarketHours& hoursInfo, uint64_t current_ts_ms) {
    using namespace std::chrono;

    system_clock::time_point tick_time{milliseconds{current_ts_ms}};
    auto tz = locate_zone("America/New_York");
    auto local_time = tz->to_local(tick_time);

    auto day = floor<days>(local_time);
    year_month_day ymd{day};
    std::string targetDate = std::format("{:04}{:02}{:02}", 
        static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));

    std::string searchPrefix = targetDate + ":";
    size_t pos = hoursInfo.liquidHoursStr.find(searchPrefix);

    auto end_of_day_local = local_days{ymd} + days{1};
    uint64_t end_of_day_ms = duration_cast<milliseconds>(tz->to_sys(end_of_day_local).time_since_epoch()).count();

    hoursInfo.calendar_day_end_ms = end_of_day_ms;

    if (pos == std::string::npos) {
        LOG_WARN("[IbkrDataFeed] Schedule not found for {}. Raw string: {}. Assuming CLOSED.", targetDate, hoursInfo.liquidHoursStr);
        hoursInfo.isClosedAllDay = true;
        hoursInfo.rth_end_ms = end_of_day_ms; 
        return;
    }

    // Safely extract the segment, handling the case where there is no trailing semicolon
    size_t startPos = pos + searchPrefix.length();
    size_t endPos = hoursInfo.liquidHoursStr.find(";", startPos);
    
    std::string hoursSegment;
    if (endPos == std::string::npos) {
        hoursSegment = hoursInfo.liquidHoursStr.substr(startPos);
    } else {
        hoursSegment = hoursInfo.liquidHoursStr.substr(startPos, endPos - startPos);
    }

    LOG_DEBUG("[IbkrDataFeed] Parsed Segment for {} ({}): '{}'", symbol, targetDate, hoursSegment);

    if (hoursSegment == "CLOSED") {
        hoursInfo.isClosedAllDay = true;
        hoursInfo.rth_end_ms = end_of_day_ms;
        return;
    }

    // Parse dynamically by finding the '-' instead of enforcing a strict 9-character length
    size_t dashPos = hoursSegment.find("-");
    if (dashPos != std::string::npos && hoursSegment.length() >= 9) {
        hoursInfo.isClosedAllDay = false;
        
        try {
            std::string start_str = hoursSegment.substr(0, 4);
            std::string end_str = hoursSegment.substr(hoursSegment.length() - 4, 4);

            int start_h = std::stoi(start_str.substr(0, 2));
            int start_m = std::stoi(start_str.substr(2, 2));
            int end_h = std::stoi(end_str.substr(0, 2));
            int end_m = std::stoi(end_str.substr(2, 2));

            auto start_local = local_days{ymd} + hours{start_h} + minutes{start_m};
            auto end_local = local_days{ymd} + hours{end_h} + minutes{end_m};

            hoursInfo.rth_start_ms = duration_cast<milliseconds>(tz->to_sys(start_local).time_since_epoch()).count();
            hoursInfo.rth_end_ms = duration_cast<milliseconds>(tz->to_sys(end_local).time_since_epoch()).count();
            
            LOG_DEBUG("[IbkrDataFeed] Active RTH set for {}: start_ms={}, end_ms={}", 
                      targetDate, hoursInfo.rth_start_ms, hoursInfo.rth_end_ms);
        } catch (const std::exception& e) {
            LOG_ERROR("[IbkrDataFeed] Failed to parse hours: '{}'. Error: {}", hoursSegment, e.what());
            hoursInfo.isClosedAllDay = true;
            hoursInfo.rth_end_ms = end_of_day_ms;
        }
    } else {
        LOG_ERROR("[IbkrDataFeed] Unrecognized hours format: '{}'", hoursSegment);
        hoursInfo.isClosedAllDay = true;
        hoursInfo.rth_end_ms = end_of_day_ms;
    }
}

bool IbkrDataFeed::isWithinRTH(const std::string& symbol, uint64_t tick_timestamp_ms) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto it = symbolMarketHours_.find(symbol);
    
    // If the hours haven't arrived from IBKR yet, drop the tick to be safe.
    if (it == symbolMarketHours_.end()) return false; 

    auto& hours = it->second;

    // Check if we need to update our daily limits (e.g., tick is from a new day)
    // Note: rth_end_ms is 0 if it hasn't been parsed yet.
    if (hours.calendar_day_end_ms == 0 || tick_timestamp_ms > hours.calendar_day_end_ms) {
        updateDailyHoursLimits(symbol, hours, tick_timestamp_ms);
    }

    if (hours.isClosedAllDay) return false;

    return (tick_timestamp_ms >= hours.rth_start_ms && tick_timestamp_ms <= hours.rth_end_ms);
}

} // namespace bot
