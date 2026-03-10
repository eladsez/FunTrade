#include <data/AlpacaRtDataFeed.hpp>
#include <core/Logger.hpp>
#include <algorithm>
#include <time.h>

namespace bot {

AlpacaRtDataFeed::AlpacaRtDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret)
    : eventBus_(std::move(eventBus)), apiKey_(std::move(apiKey)), 
    apiSecret_(std::move(apiSecret)) {

    infra_ = std::make_unique<AlpacaInfra>(apiKey_, apiSecret_);
    
    // Register our parser directly with the Infra layer
    infra_->setWsMessageCallback([this](const std::string& msg) {
        onWebSocketMessage(msg);
    });

    if (!connectToInfra()){
        throw std::runtime_error("[AlpacaRtDataFeed] Failed connecting to Infra");
    }
}

AlpacaRtDataFeed::~AlpacaRtDataFeed() {
    disconnectFromInfra();
}

bool AlpacaRtDataFeed::connectToInfra() {
    // Blocks until network connection AND Alpaca authentication are successful
    bool success = infra_->connectWs("stream.data.alpaca.markets", "/v2/iex", WsType::DATA);
    if (!success) {
        LOG_ERROR("[AlpacaRtDataFeed] Failed to connect and authenticate.");
    }
    return success;
}

void AlpacaRtDataFeed::disconnectFromInfra() {
    infra_->disconnectWs();
}

void AlpacaRtDataFeed::subscribeToSymbol(const std::string& symbol) {
    nlohmann::json subPayload = {
        {"action", "subscribe"},
        {"trades", {symbol}},
        {"quotes", {symbol}},
        {"orderbooks", {symbol}}
    };
    
    LOG_INFO("[AlpacaRtDataFeed] Subscribing to {}", symbol);
    infra_->sendWsMessage(subPayload.dump());
}

void AlpacaRtDataFeed::onWebSocketMessage(const std::string& payload) {
    if (payload.empty()) return;
    
    // Debug: Print raw payload to see what Alpaca is sending
    // LOG_DEBUG("[AlpacaRtDataFeed] WS Received: {}", payload);
    
    try {
        auto msgArray = nlohmann::json::parse(payload);
        
        for (const auto& msg : msgArray) {
            std::string msgType = msg.value("T", "");

            if (msgType == "t") {
                parseTradeUpdate(msg);
            } else if (msgType == "q") {
                parseQuoteUpdate(msg);
            } else if (msgType == "o") {
                parseOrderBookUpdate(msg);
            } else if (msgType == "error") {
                LOG_ERROR("[AlpacaRtDataFeed] Stream error: {}", msg.dump());
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        LOG_ERROR("[AlpacaRtDataFeed] JSON Parse Error: {}", e.what());
    }
}

// Parses an RFC3339 UTC string into milliseconds since the epoch
inline uint64_t parseAlpacaTimestamp(const std::string& ts) {
    if (ts.empty()) return 0;

    struct tm t{};
    // Parse the base date and time
    char* p = strptime(ts.c_str(), "%Y-%m-%dT%H:%M:%S", &t);
    if (p == nullptr) return 0;

    // CRITICAL: Use timegm, not std::mktime. 
    // std::mktime applies your machine's local timezone offset, which will corrupt the UTC time.
    time_t seconds = timegm(&t);
    uint64_t ms = static_cast<uint64_t>(seconds) * 1000;

    // Parse fractional seconds dynamically (Alpaca can send variable precision)
    if (*p == '.') {
        p++;
        uint64_t fraction = 0;
        int digitCount = 0;
        
        while (*p >= '0' && *p <= '9') {
            fraction = fraction * 10 + (*p - '0');
            digitCount++;
            p++;
        }
        
        // Normalize the extracted fraction to exactly 3 digits (milliseconds)
        if (digitCount > 0) {
            if (digitCount >= 3) {
                for (int i = 0; i < digitCount - 3; ++i) fraction /= 10;
                ms += fraction;
            } else {
                for (int i = 0; i < 3 - digitCount; ++i) fraction *= 10;
                ms += fraction;
            }
        }
    }
    return ms;
}

void AlpacaRtDataFeed::parseTradeUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaRtDataFeed] TradeUpdate Received: {}", data);
    try {
        TradeTick tick; 
        
        std::string rawSymbol = data.at("S").get<std::string>();
        rawSymbol.erase(std::remove(rawSymbol.begin(), rawSymbol.end(), '/'), rawSymbol.end());
        tick.symbol = rawSymbol;
        tick.price = data.at("p").get<double>();
        tick.volume = data.at("s").get<double>();
        
        // Parse the timestamp string to epoch milliseconds
        std::string tsString = data.value("t", "");
        tick.timestamp_ms = parseAlpacaTimestamp(tsString);

        // Parse Taker Side (if available, mostly for Crypto)
        if (data.contains("tks")) {
            tick.taker_side = stringToSide(data.at("tks").get<std::string>());
        }

        // Parse Trade Conditions (if available, mostly for Stocks)
        if (data.contains("c") && data.at("c").is_array()) {
            for (const auto& cond : data.at("c")) {
                tick.conditions.push_back(stringToTradeCondition(cond.get<std::string>()));
            }
        }

        if (eventBus_) {
            eventBus_->publishMarketEvent(tick); 
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing trade: {}", e.what());
    }
}

void AlpacaRtDataFeed::parseQuoteUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaRtDataFeed] QuoteUpdate Received: {}", data);
    try {
        QuoteTick tick; 
        
        std::string rawSymbol = data.at("S").get<std::string>();
        rawSymbol.erase(std::remove(rawSymbol.begin(), rawSymbol.end(), '/'), rawSymbol.end());
        tick.symbol = rawSymbol;
        
        tick.ask_price = data.at("ap").get<double>();
        tick.ask_size = data.at("as").get<double>();
        tick.bid_price = data.at("bp").get<double>();
        tick.bid_size = data.at("bs").get<double>();
        
        // Parse the timestamp string to epoch milliseconds
        std::string tsString = data.value("t", "");
        tick.timestamp_ms = parseAlpacaTimestamp(tsString);

        // Parse Quote Conditions
        if (data.contains("c") && data.at("c").is_array()) {
            for (const auto& cond : data.at("c")) {
                tick.conditions.push_back(stringToQuoteCondition(cond.get<std::string>()));
            }
        }

        if (eventBus_) {
            eventBus_->publishMarketEvent(tick);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing quote: {}", e.what());
    }
}

void AlpacaRtDataFeed::parseOrderBookUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaRtDataFeed] BookUpdate Received: {}", data);
    try {
        OrderBookUpdate update; // Changed from MarketTick
        
        std::string rawSymbol = data.at("S").get<std::string>();
        rawSymbol.erase(std::remove(rawSymbol.begin(), rawSymbol.end(), '/'), rawSymbol.end());
        update.symbol = rawSymbol;
        
        // Parse the timestamp string to epoch milliseconds
        std::string tsString = data.value("t", "");
        update.timestamp_ms = parseAlpacaTimestamp(tsString);

        // Capture the reset/snapshot flag (defaulting to false if not present)
        update.is_snapshot = data.value("r", false);

        const auto& bids = data.at("b");
        const auto& asks = data.at("a");

        // Iterate through the full array of bids to capture market depth
        for (const auto& b : bids) {
            update.bids.push_back({
                b.at("p").get<double>(), 
                b.at("s").get<double>()
            });
        }

        // Iterate through the full array of asks to capture market depth
        for (const auto& a : asks) {
            update.asks.push_back({
                a.at("p").get<double>(), 
                a.at("s").get<double>()
            });
        }

        if (eventBus_) {
            eventBus_->publishMarketEvent(update);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing orderbook: {}", e.what());
    }
}

Side AlpacaRtDataFeed::stringToSide(const std::string& side) {
        if (side == "B") return Side::BUY;
        if (side == "S") return Side::SELL;
        return Side::UNKNOWN;
    }

TradeCondition AlpacaRtDataFeed::stringToTradeCondition(const std::string& cond) {
    if (cond == "@" || cond == " ") return TradeCondition::REGULAR;
    if (cond == "F") return TradeCondition::ISO;
    if (cond == "I") return TradeCondition::ODD_LOT;
    if (cond == "U") return TradeCondition::EXTENDED_HOURS;
    if (cond == "Z") return TradeCondition::LATE_PRINT;
    if (cond == "C") return TradeCondition::CASH_TRADE;
    if (cond == "P") return TradeCondition::PRIOR_REFERENCE;
    return TradeCondition::OTHER;
}

QuoteCondition AlpacaRtDataFeed::stringToQuoteCondition(const std::string& cond) {
    if (cond == "R") return QuoteCondition::NON_FIRM;
    if (cond == "C") return QuoteCondition::CLOSING;
    return QuoteCondition::OTHER;
}

std::vector<HistoricalBar> AlpacaRtDataFeed::fetchHistoricalBars(
    const std::string& symbol, 
    const std::string& start, 
    const std::string& end, 
    const std::string& timeframe) {
    
    std::vector<HistoricalBar> bars;

    // Construct the endpoint for Alpaca Market Data V2 Stocks
    std::string target = "/v2/stocks/bars?symbols=" + symbol + 
                         "&timeframe=" + timeframe + 
                         "&start=" + start + 
                         "&end=" + end;

    try {
        // Alpaca's data API uses data.alpaca.markets
        LOG_INFO("[AlpacaRtDataFeed] Fetching historical {} bars for {}", timeframe, symbol);
        std::string response = infra_->sendRestRequest("GET", "data.alpaca.markets", target);

        if (response.empty()) {
            LOG_ERROR("[AlpacaRtDataFeed] Received empty response for historical bars.");
            return bars;
        }

        auto jsonResp = nlohmann::json::parse(response);

        // Check if Alpaca returned an error message
        if (jsonResp.contains("code") && jsonResp.contains("message")) {
            LOG_ERROR("[AlpacaRtDataFeed] API Error: {}", jsonResp["message"].get<std::string>());
            return bars;
        }

        // Parse the bars out of the JSON structure
        if (jsonResp.contains("bars") && jsonResp["bars"].contains(symbol)) {
            for (const auto& item : jsonResp["bars"][symbol]) {
                HistoricalBar bar;
                bar.symbol = symbol;
                
                // Reuse your existing timestamp parser
                bar.timestamp_ms = parseAlpacaTimestamp(item.value("t", ""));
                
                bar.open = item.value("o", 0.0);
                bar.high = item.value("h", 0.0);
                bar.low = item.value("l", 0.0);
                bar.close = item.value("c", 0.0);
                bar.volume = item.value("v", 0.0);
                bar.trade_count = item.value("n", 0ull);
                bar.vwap = item.value("vw", 0.0);
                
                bars.push_back(bar);
            }
            LOG_INFO("[AlpacaRtDataFeed] Successfully parsed {} historical bars for {}", bars.size(), symbol);
        } else {
            LOG_WARN("[AlpacaRtDataFeed] No bars found in response for {}", symbol);
        }

    } catch (const nlohmann::json::parse_error& e) {
        LOG_ERROR("[AlpacaRtDataFeed] JSON Parse Error in fetchHistoricalBars: {}", e.what());
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Exception in fetchHistoricalBars: {}", e.what());
    }

    return bars;
}

} // namespace bot