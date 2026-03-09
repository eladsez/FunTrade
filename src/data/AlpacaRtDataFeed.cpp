#include <data/AlpacaRtDataFeed.hpp>
#include <core/Logger.hpp>
#include <algorithm>
#include <time.h>

namespace bot {

AlpacaRtDataFeed::AlpacaRtDataFeed(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<AlpacaInfra> infra, std::string apiKey, std::string apiSecret)
    : eventBus_(std::move(eventBus)), infra_(std::move(infra)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {
    
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
    bool success = infra_->connectWs("stream.data.alpaca.markets", "/v1beta3/crypto/us", WsType::DATA);
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
        MarketTick tick;
        tick.type = TickType::TRADE;
        std::string rawSymbol = data.at("S").get<std::string>();
        rawSymbol.erase(std::remove(rawSymbol.begin(), rawSymbol.end(), '/'), rawSymbol.end());
        tick.symbol = rawSymbol;
        tick.price = data.at("p").get<double>();
        tick.volume = data.at("s").get<double>();
        
        // Parse the timestamp string to epoch milliseconds
        std::string tsString = data.value("t", "");
        tick.timestamp_ms = parseAlpacaTimestamp(tsString);

        if (eventBus_) {
            eventBus_->publishTick(tick);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing trade: {}", e.what());
    }
}

void AlpacaRtDataFeed::parseQuoteUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaRtDataFeed] QuoteUpdate Received: {}", data);
    try {
        MarketTick tick;
        tick.type = TickType::QUOTE;
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

        if (eventBus_) {
            eventBus_->publishTick(tick);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing quote: {}", e.what());
    }
}

void AlpacaRtDataFeed::parseOrderBookUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaRtDataFeed] BookUpdate Received: {}", data);
    try {
        MarketTick tick;
        tick.type = TickType::ORDERBOOK;
        std::string rawSymbol = data.at("S").get<std::string>();
        rawSymbol.erase(std::remove(rawSymbol.begin(), rawSymbol.end(), '/'), rawSymbol.end());
        tick.symbol = rawSymbol;
        
        const auto& bids = data.at("b");
        const auto& asks = data.at("a");

        if (!bids.empty() && !asks.empty()) {
            tick.ask_price = asks[0].at("p").get<double>();
            tick.ask_size = asks[0].at("s").get<double>();
            tick.bid_price = bids[0].at("p").get<double>();
            tick.bid_size = bids[0].at("s").get<double>();
            // Parse the timestamp string to epoch milliseconds
            std::string tsString = data.value("t", "");
            tick.timestamp_ms = parseAlpacaTimestamp(tsString);

            if (eventBus_) {
                eventBus_->publishTick(tick);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaRtDataFeed] Error parsing orderbook: {}", e.what());
    }
}

} // namespace bot