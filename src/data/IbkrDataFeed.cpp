#include <data/IbkrDataFeed.hpp>
#include <core/Logger.hpp>
#include <chrono>

// TWS API Includes
#include <Decimal.h> 

namespace bot {

IbkrDataFeed::IbkrDataFeed(std::shared_ptr<IbkrInfra> infra, IEventBus* eventBus)
    : infra_(std::move(infra)), eventBus_(eventBus) {}

bool IbkrDataFeed::connectToInfra() {
    LOG_INFO("[IbkrDataFeed] Hooking up market data callbacks to Infra...");
    
    infra_->setTickPriceCallback(
        [this](TickerId id, TickType type, double price, const TickAttrib& attr) {
            this->onTickPrice(id, type, price, attr);
        }
    );

    infra_->setTickSizeCallback(
        [this](TickerId id, TickType type, Decimal size) {
            this->onTickSize(id, type, size);
        }
    );

    return true; // Interface expects bool indicating success
}

void IbkrDataFeed::disconnectFromInfra() {
    LOG_INFO("[IbkrDataFeed] Disconnecting DataFeed from Infra.");
    // In our architecture, the actual network disconnect happens in IbkrInfra.
}

void IbkrDataFeed::subscribeToSymbol(const std::string& symbol) {
    if (!infra_->getClient() || !infra_->getClient()->isConnected()) {
        LOG_ERROR("[IbkrDataFeed] Cannot subscribe. Not connected to TWS.");
        return;
    }

    int tickerId = getOrCreateTickerId(symbol);
    Contract contract = createContract(symbol);

    LOG_INFO("[IbkrDataFeed] Subscribing to market data for {} (ID: {})", symbol, tickerId);
    
    // reqMktData parameters: tickerId, contract, genericTicks, snapshot, regulatorySnapshot, mktDataOptions
    infra_->getClient()->reqMktData(tickerId, contract, "", false, false, nullptr);
}

void IbkrDataFeed::unsubscribeFromSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto it = symbolToId_.find(symbol);
    if (it != symbolToId_.end()) {
        int tickerId = it->second;
        infra_->getClient()->cancelMktData(tickerId);
        LOG_INFO("[IbkrDataFeed] Unsubscribed from {}", symbol);
    }
}

std::vector<std::string> IbkrDataFeed::getActiveSubscriptions() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<std::string> active;
    for (const auto& pair : symbolToId_) {
        active.push_back(pair.first);
    }
    return active;
}

void IbkrDataFeed::setMarketTickCallback(std::function<void(const MarketEvent&)> callback) {
    tickCallback_ = std::move(callback);
}

// =========================================================================
// The Stateful Cache & Callback Processors
// =========================================================================

void IbkrDataFeed::onTickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib) {
    (void)attrib; // Suppress unused parameter warning

    std::lock_guard<std::mutex> lock(dataMutex_);
    std::string symbol = getSymbolFromId(tickerId);
    if (symbol.empty()) return; // Unknown ID

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();

    // 1 = BID, 2 = ASK
    if (field == TickType::BID || field == TickType::ASK) {
        QuoteTick& quote = quoteCache_[tickerId];
        quote.symbol = symbol;
        quote.timestamp_ms = nowMs;
        
        if (field == 1) quote.bid_price = price;
        if (field == 2) quote.ask_price = price;

        if (quote.bid_price > 0 && quote.ask_price > 0) {
            if (eventBus_) eventBus_->publishMarketEvent(quote);
            if (tickCallback_) tickCallback_(quote);
        }
    }
    // 4 = LAST (Trade)
    else if (field == TickType::LAST) {
        TradeTick& trade = tradeCache_[tickerId];
        trade.symbol = symbol;
        trade.timestamp_ms = nowMs;
        trade.price = price;
        
        if (trade.volume > 0) { // Changed size to volume
            if (eventBus_) eventBus_->publishMarketEvent(trade);
            if (tickCallback_) tickCallback_(trade);
        }
    }
}

void IbkrDataFeed::onTickSize(TickerId tickerId, TickType field, Decimal size) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::string symbol = getSymbolFromId(tickerId);
    if (symbol.empty()) return;

    // Use DecimalFunctions namespace for TWS API
    double sizeDbl = (size == UNSET_DECIMAL) ? 0.0 : DecimalFunctions::decimalToDouble(size); 

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();

    // 0 = BID_SIZE, 3 = ASK_SIZE
    if (field == TickType::BID_SIZE || field == TickType::ASK_SIZE) {
        QuoteTick& quote = quoteCache_[tickerId];
        quote.symbol = symbol;
        quote.timestamp_ms = nowMs;

        if (field == TickType::BID_SIZE) quote.bid_size = sizeDbl;
        if (field == TickType::ASK_SIZE) quote.ask_size = sizeDbl;

        if (quote.bid_price > 0 && quote.ask_price > 0) {
            if (eventBus_) eventBus_->publishMarketEvent(quote);
            if (tickCallback_) tickCallback_(quote);
        }
    }
    // 5 = LAST_SIZE (Trade Size)
    else if (field == TickType::LAST_SIZE) {
        TradeTick& trade = tradeCache_[tickerId];
        trade.symbol = symbol;
        trade.timestamp_ms = nowMs;
        trade.volume = sizeDbl; // Changed size to volume

        if (trade.price > 0) {
            if (eventBus_) eventBus_->publishMarketEvent(trade);
            if (tickCallback_) tickCallback_(trade);
        }
    }
}

// =========================================================================
// Helpers
// =========================================================================

int IbkrDataFeed::getOrCreateTickerId(const std::string& symbol) {
    if (symbolToId_.find(symbol) == symbolToId_.end()) {
        int newId = nextTickerId_++;
        symbolToId_[symbol] = newId;
        idToSymbol_[newId] = symbol;
    }
    return symbolToId_[symbol];
}

std::string IbkrDataFeed::getSymbolFromId(int tickerId) {
    auto it = idToSymbol_.find(tickerId);
    if (it != idToSymbol_.end()) {
        return it->second;
    }
    return "";
}

Contract IbkrDataFeed::createContract(const std::string& symbol) {
    Contract contract;
    contract.symbol = symbol;
    contract.secType = "STK";
    contract.currency = "USD";
    contract.exchange = "SMART"; 
    return contract;
}

std::vector<HistoricalBar> IbkrDataFeed::fetchHistoricalBars(const std::string& symbol, const std::string& start, const std::string& end, const std::string& timeframe) {
    // Suppress unused parameter warnings for now
    (void)symbol; 
    (void)start; 
    (void)end; 
    (void)timeframe;

    LOG_WARN("[IbkrDataFeed] fetchHistoricalBars not yet implemented.");
    return {};
}

} // namespace bot