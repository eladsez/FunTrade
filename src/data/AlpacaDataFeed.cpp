#include <data/AlpacaDataFeed.hpp>
#include <iostream>
#include <algorithm>

namespace bot {

AlpacaDataFeed::AlpacaDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret)
    : eventBus_(std::move(eventBus)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {}

AlpacaDataFeed::~AlpacaDataFeed() {
    disconnect();
}

bool AlpacaDataFeed::connect() {
    if (isConnected_) return true;

    // In reality: WS connect, wait for async callback, then send auth payload
    std::cout << "[AlpacaDataFeed] Connecting to WebSocket...\n";
    
    nlohmann::json authPayload = {
        {"action", "auth"},
        {"key", apiKey_},
        {"secret", apiSecret_}
    };
    sendWebSocketMessage(authPayload.dump());
    
    isConnected_ = true;
    return true;
}

void AlpacaDataFeed::subscribeToSymbol(const std::string& symbol) {
    if (!isConnected_) {
        std::cerr << "[AlpacaDataFeed] Cannot subscribe, not connected.\n";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(symbolsMutex_);
        if (std::find(subscribedSymbols_.begin(), subscribedSymbols_.end(), symbol) == subscribedSymbols_.end()) {
            subscribedSymbols_.push_back(symbol);
        }
    }

    nlohmann::json subPayload = {
        {"action", "subscribe"},
        {"trades", {symbol}},
        {"quotes", {symbol}},
        {"orderbooks", {symbol}} // Assume custom implementation handles orderbooks if supported
    };

    std::cout << "[AlpacaDataFeed] Sending Subscription: " << subPayload.dump() << "\n";
    sendWebSocketMessage(subPayload.dump());
}

void AlpacaDataFeed::disconnect() {
    if (!isConnected_.exchange(false)) return;
    std::cout << "[AlpacaDataFeed] Disconnecting WebSocket.\n";
    // WS teardown logic here
}

void AlpacaDataFeed::onWebSocketMessage(const std::string& payload) {
    if (payload.empty()) return;

    try {
        auto msgArray = nlohmann::json::parse(payload);
        
        // Alpaca sends an array of messages
        for (const auto& msg : msgArray) {
            std::string msgType = msg.value("T", "");

            if (msgType == "t") {
                parseTradeUpdate(msg);
            } else if (msgType == "q") {
                parseQuoteUpdate(msg);
            } else if (msgType == "b") {
                parseOrderBookUpdate(msg);
            } else if (msgType == "error") {
                std::cerr << "[AlpacaDataFeed] Received error: " << msg.value("msg", "Unknown error") << "\n";
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[AlpacaDataFeed] JSON Parse Error: " << e.what() << "\nPayload: " << payload << "\n";
    }
}

void AlpacaDataFeed::parseTradeUpdate(const nlohmann::json& data) {
    try {
        MarketTick tick;
        tick.symbol = data.at("S").get<std::string>();
        tick.price = data.at("p").get<double>();
        tick.volume = data.at("s").get<double>();
        // Using current time as mock timestamp for PoC; usually data.at("t")
        tick.timestamp_ms = 0; 

        if (eventBus_) {
            eventBus_->publishTick(tick);
        }
    } catch (const std::exception& e) {
        std::cerr << "[AlpacaDataFeed] Error parsing trade: " << e.what() << "\n";
    }
}

void AlpacaDataFeed::parseQuoteUpdate(const nlohmann::json& data) {
    (void)data; // Suppress unused parameter warning
    // Logic for Level 1 Quotes (Bid/Ask)
    // std::cout << "[AlpacaDataFeed] Quote Update parsed.\n";
}

void AlpacaDataFeed::parseOrderBookUpdate(const nlohmann::json& data) {
    (void)data; // Suppress unused parameter warning
    // Logic for L2 Order Book Imbalance calculation preparations
    // std::cout << "[AlpacaDataFeed] OrderBook Update parsed.\n";
}

void AlpacaDataFeed::sendWebSocketMessage(const std::string& message) {
    (void)message; // Suppress unused parameter warning
    // Delegate to WS Client (e.g., Boost.Beast)
    // std::cout << "[AlpacaDataFeed->Network] " << message << "\n";
}

} // namespace bot
