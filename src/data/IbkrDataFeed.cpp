#include <data/IbkrDataFeed.hpp>
#include <core/Logger.hpp>
#include <Decimal.h> 

namespace bot {

IbkrDataFeed::IbkrDataFeed(std::shared_ptr<IbkrInfra> infra)
    : infra_(std::move(infra)) {}

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

    return true;
}

void IbkrDataFeed::disconnectFromInfra() {
    LOG_INFO("[IbkrDataFeed] Disconnecting DataFeed from Infra.");
}

void IbkrDataFeed::subscribeToSymbol(const std::string& symbol) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) {
        LOG_ERROR("[IbkrDataFeed] Cannot subscribe. Not connected to TWS.");
        return;
    }

    int bidAskId, allLastId, midPointId;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (symbolToIds_.find(symbol) != symbolToIds_.end()) return; // Already subscribed

        // Allocate two unique IDs for this symbol
        bidAskId = nextTickerId_++;
        allLastId = nextTickerId_++;
        midPointId = nextTickerId_++;

        symbolToIds_[symbol] = {bidAskId, allLastId, midPointId};
        idToSymbol_[bidAskId] = symbol;
        idToSymbol_[allLastId] = symbol;
        idToSymbol_[midPointId] = symbol;
    }

    Contract contract = createContract(symbol);

    LOG_INFO("[IbkrDataFeed] Requesting Tick-By-Tick data for {}", symbol);
    
    // Request Quotes (0 = numberOfTicks (not used for real-time), false = ignoreSize)
    infra_->getClient()->reqTickByTickData(bidAskId, contract, "BidAsk", 0, false);
    
    // Request Trades (AllLast includes unreported/late trades, Last is strictly regular)
    infra_->getClient()->reqTickByTickData(allLastId, contract, "AllLast", 0, false);

    // Request MidPoint
    infra_->getClient()->reqTickByTickData(midPointId, contract, "MidPoint", 0, false);
}

void IbkrDataFeed::unsubscribeFromSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto it = symbolToIds_.find(symbol);
    
    if (it != symbolToIds_.end()) {
        // 1. Cancel all three streams
        if (infra_->getClient() && infra_->getClient()->isConnected()) {
            infra_->getClient()->cancelTickByTickData(it->second.bidAskId);
            infra_->getClient()->cancelTickByTickData(it->second.allLastId);
            infra_->getClient()->cancelTickByTickData(it->second.midPointId);
        }

        // 2. Clean up the reverse lookup maps to prevent leaks
        idToSymbol_.erase(it->second.bidAskId);
        idToSymbol_.erase(it->second.allLastId);
        idToSymbol_.erase(it->second.midPointId);

        // 3. Remove from the main tracking map
        symbolToIds_.erase(it);

        LOG_INFO("[IbkrDataFeed] Unsubscribed from Tick-By-Tick data for {}", symbol);
    }
}

std::vector<std::string> IbkrDataFeed::getActiveSubscriptions() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<std::string> active;
    for (const auto& pair : symbolToIds_) {
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
    (void)symbol; (void)start; (void)end; (void)timeframe;
    LOG_WARN("[IbkrDataFeed] fetchHistoricalBars not yet implemented.");
    return {};
}

} // namespace bot