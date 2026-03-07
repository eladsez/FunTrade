#include <data/AlpacaDataFeed.hpp>
#include <core/Logger.hpp>
#include <algorithm>
#include <chrono>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace bot {

struct WsClientState {
    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws;
};

AlpacaDataFeed::AlpacaDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret)
    : eventBus_(std::move(eventBus)), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)), wsState_(std::make_unique<WsClientState>()) {
    wsState_->ctx.set_default_verify_paths();
}

AlpacaDataFeed::~AlpacaDataFeed() {
    disconnect();
}

bool AlpacaDataFeed::connect() {
    if (isConnected_) return true;

    LOG_INFO("[AlpacaDataFeed] Connecting to WebSocket...");
    isRunning_ = true;
    wsThread_ = std::thread(&AlpacaDataFeed::wsRunLoop, this);

    // Wait briefly for the connection flag to flip (sufficient for PoC)
    // In a production system, use a std::condition_variable.
    int attempts = 0;
    while (!isConnected_ && isRunning_ && attempts < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        attempts++;
    }

    if (!isConnected_) {
        LOG_ERROR("[AlpacaDataFeed] Connection timed out.");
        disconnect();
    }
    
    return isConnected_;
}

void AlpacaDataFeed::subscribeToSymbol(const std::string& symbol) {
    if (!isConnected_) {
        LOG_ERROR("[AlpacaDataFeed] Cannot subscribe, not connected.");
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
        {"orderbooks", {symbol}} // If supported by the specific data source
    };

    LOG_INFO("[AlpacaDataFeed] Sending Subscription: {}", subPayload.dump());
    sendWebSocketMessage(subPayload.dump());
}

void AlpacaDataFeed::disconnect() {
    if (!isRunning_.exchange(false)) return;

    if (isConnected_ && wsState_->ws && wsState_->ws->is_open()) {
        try {
            beast::error_code ec;
            // Forcefully close the underlying TCP socket to unblock any pending read operations.
            // Calling websocket::close() concurrently with a blocking read() is not thread-safe in Boost.Beast.
            beast::get_lowest_layer(*wsState_->ws).cancel(ec);
            beast::get_lowest_layer(*wsState_->ws).close(ec);
        } catch(...) {}
    }
    
    isConnected_ = false;

    if (wsThread_.joinable()) {
        wsThread_.join();
    }
    LOG_INFO("[AlpacaDataFeed] Disconnected WebSocket.");
}

void AlpacaDataFeed::wsRunLoop() {
    try {
        std::string host = "stream.data.alpaca.markets";
        std::string port = "443";
        std::string path = "/v1beta3/crypto/us"; // Switching to Crypto stream for 24/7 live testing

        tcp::resolver resolver(wsState_->ioc);
        wsState_->ws = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(net::make_strand(wsState_->ioc), wsState_->ctx);

        auto const results = resolver.resolve(host, port);
        net::connect(beast::get_lowest_layer(*wsState_->ws), results);

        // Set SNI Hostname
        if(! SSL_set_tlsext_host_name(wsState_->ws->next_layer().native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        wsState_->ws->next_layer().handshake(ssl::stream_base::client);
        wsState_->ws->handshake(host, path);

        isConnected_ = true;
        LOG_INFO("[AlpacaDataFeed] WebSocket Connected. Authenticating...");

        nlohmann::json authPayload = {
            {"action", "auth"},
            {"key", apiKey_},
            {"secret", apiSecret_}
        };
        sendWebSocketMessage(authPayload.dump());

        beast::flat_buffer buffer;
        while (isRunning_ && wsState_->ws->is_open()) {
            wsState_->ws->read(buffer);
            std::string payload = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            onWebSocketMessage(payload);
        }

    } catch (const std::exception& e) {
        if (isRunning_) {
            LOG_ERROR("[AlpacaDataFeed] WebSocket Run Loop Error: {}", e.what());
        }
        isConnected_ = false;
        isRunning_ = false;
    }
}

void AlpacaDataFeed::sendWebSocketMessage(const std::string& message) {
    if (isConnected_ && wsState_->ws && wsState_->ws->is_open()) {
        try {
            wsState_->ws->write(net::buffer(message));
        } catch (const std::exception& e) {
            LOG_ERROR("[AlpacaDataFeed] Send Error: {}", e.what());
        }
    }
}

void AlpacaDataFeed::onWebSocketMessage(const std::string& payload) {
    if (payload.empty()) return;
    
    // Debug: Print raw payload to see what Alpaca is sending
    // LOG_DEBUG("[DEBUG] WS Received: {}", payload);

    try {
        auto msgArray = nlohmann::json::parse(payload);
        
        for (const auto& msg : msgArray) {
            std::string msgType = msg.value("T", "");

            if (msgType == "success" && msg.value("msg", "") == "authenticated") {
                LOG_INFO("[AlpacaDataFeed] Authentication SUCCESSFUL.");
            } else if (msgType == "t") {
                parseTradeUpdate(msg);
            } else if (msgType == "q") {
                parseQuoteUpdate(msg);
            } else if (msgType == "o") {
                parseOrderBookUpdate(msg);
            } else if (msgType == "error") {
                LOG_ERROR("[AlpacaDataFeed] Received error: {}", msg.dump());
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        LOG_ERROR("[AlpacaDataFeed] JSON Parse Error: {}\nPayload: {}", e.what(), payload);
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
        LOG_ERROR("[AlpacaDataFeed] Error parsing trade: {}", e.what());
    }
}

void AlpacaDataFeed::parseQuoteUpdate(const nlohmann::json& data) {
    try {
        MarketTick tick;
        tick.symbol = data.at("S").get<std::string>();
        tick.price = data.at("ap").get<double>(); // Using Ask Price as current price reference
        tick.bid_size = data.at("bs").get<double>();
        tick.ask_size = data.at("as").get<double>();
        tick.volume = 0; // Quotes don't have trade volume
        tick.timestamp_ms = 0;

        if (eventBus_) {
            eventBus_->publishTick(tick);
        }
    } catch (const std::exception& e) {
        // LOG_ERROR("[AlpacaDataFeed] Error parsing quote: {}", e.what());
    }
}

void AlpacaDataFeed::parseOrderBookUpdate(const nlohmann::json& data) {
    try {
        MarketTick tick;
        tick.symbol = data.at("S").get<std::string>();
        
        const auto& bids = data.at("b");
        const auto& asks = data.at("a");

        if (!bids.empty() && !asks.empty()) {
            // Top of book is the first element
            tick.price = asks[0].at("p").get<double>();
            tick.bid_size = bids[0].at("s").get<double>();
            tick.ask_size = asks[0].at("s").get<double>();
            tick.volume = 0;
            tick.timestamp_ms = 0;

            if (eventBus_) {
                eventBus_->publishTick(tick);
            }
        }
    } catch (const std::exception& e) {
        // LOG_ERROR("[AlpacaDataFeed] Error parsing orderbook: {}", e.what());
    }
}

} // namespace bot