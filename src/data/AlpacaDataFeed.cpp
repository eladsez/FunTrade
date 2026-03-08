#include <data/AlpacaDataFeed.hpp>
#include <core/Logger.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <time.h>

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
    beast::flat_buffer buffer;
    
    std::deque<std::string> writeQueue;
    bool isWriting = false;
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
    
    std::unique_lock<std::mutex> lock(connectMutex_);
    wsThread_ = std::thread(&AlpacaDataFeed::wsRunLoop, this);

    // Wait for the connection flag to flip or error to occur
    if (connectCv_.wait_for(lock, std::chrono::seconds(10), [this] { return isConnected_.load() || !isRunning_.load(); })) {
        if (isConnected_) {
            return true;
        } else {
            LOG_ERROR("[AlpacaDataFeed] Connection failed during setup.");
            disconnect();
            return false;
        }
    } else {
        LOG_ERROR("[AlpacaDataFeed] Connection timed out.");
        disconnect();
        return false;
    }
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

    if (wsState_) {
        net::post(wsState_->ioc, [this]() {
            if (wsState_->ws && wsState_->ws->is_open()) {
                // Close the websocket gracefully asynchronously
                wsState_->ws->async_close(websocket::close_code::normal, [](beast::error_code){});
            }
        });
    }
    
    isConnected_ = false;
    connectCv_.notify_all(); // Unblock any threads waiting in connect()

    if (wsThread_.joinable()) {
        wsThread_.join();
    }
    LOG_INFO("[AlpacaDataFeed] Disconnected WebSocket.");
}

void AlpacaDataFeed::doRead() {
    if (!isRunning_ || !wsState_->ws->is_open()) return;

    wsState_->ws->async_read(wsState_->buffer, 
        [this](beast::error_code ec, std::size_t) {
            if (ec) {
                if (isRunning_) LOG_ERROR("[AlpacaDataFeed] WS Read Error: {}", ec.message());
                isConnected_ = false;
                return;
            }
            std::string payload = beast::buffers_to_string(wsState_->buffer.data());
            wsState_->buffer.consume(wsState_->buffer.size());
            
            onWebSocketMessage(payload);
            
            if (isRunning_) doRead(); // Safe, recursive call
        }
    );
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

        // Start the read loop immediately, DO NOT send auth yet
        doRead();
        
        // Block until io_context is stopped or runs out of work.
        wsState_->ioc.run();
        wsState_->ioc.restart();

    } catch (const std::exception& e) {
        if (isRunning_) {
            LOG_ERROR("[AlpacaDataFeed] WebSocket Run Loop Error: {}", e.what());
        }
        {
            std::lock_guard<std::mutex> lock(connectMutex_);
            isConnected_ = false;
            isRunning_ = false;
        }
        connectCv_.notify_all(); // Notify if it failed during handshake
    }
}

void AlpacaDataFeed::sendWebSocketMessage(const std::string& message) {
    // Check if the physical socket is open
    if (wsState_ && wsState_->ws && wsState_->ws->is_open()) {
        // Post to the WEBSOCKET'S executor (the strand)
        net::post(wsState_->ws->get_executor(), [this, msg = message]() {
            wsState_->writeQueue.push_back(msg);
            if (!wsState_->isWriting) {
                doWrite();
            }
        });
    } else {
        LOG_ERROR("[AlpacaDataFeed] Cannot send message: WebSocket is not open.");
    }
}

void AlpacaDataFeed::doWrite() {
    if (wsState_->writeQueue.empty()) {
        wsState_->isWriting = false;
        return;
    }

    wsState_->isWriting = true;
    
    // Force text framing for JSON payloads
    wsState_->ws->text(true); 

    wsState_->ws->async_write(
        net::buffer(wsState_->writeQueue.front()),
        [this](beast::error_code ec, std::size_t /*bytes_transferred*/) {
            if (ec) {
                LOG_ERROR("[AlpacaDataFeed] Send Error: {}", ec.message());
                // In case of error, we might want to clear the queue or disconnect
                wsState_->isWriting = false;
                isConnected_ = false;
                return;
            }

            // Remove the sent message
            wsState_->writeQueue.pop_front();
            
            // Send the next message
            doWrite();
        }
    );
}

void AlpacaDataFeed::onWebSocketMessage(const std::string& payload) {
    if (payload.empty()) return;
    
    // Debug: Print raw payload to see what Alpaca is sending
    // LOG_DEBUG("[AlpacaDataFeed] WS Received: {}", payload);

    try {
        auto msgArray = nlohmann::json::parse(payload);
        
        for (const auto& msg : msgArray) {
            std::string msgType = msg.value("T", "");

            // 1. Wait for the welcome message
            if (msgType == "success" && msg.value("msg", "") == "connected") {
                LOG_INFO("[AlpacaDataFeed] Server connected. Sending authentication...");
                nlohmann::json authPayload = {
                    {"action", "auth"},
                    {"key", apiKey_},
                    {"secret", apiSecret_}
                };
                sendWebSocketMessage(authPayload.dump());
            // 2. Wait for auth success
            } else if (msgType == "success" && msg.value("msg", "") == "authenticated") {
                LOG_INFO("[AlpacaDataFeed] Authentication SUCCESSFUL.");
                // NOTE: It is now safe to send your subscription payloads!
                {
                    std::lock_guard<std::mutex> lock(connectMutex_);
                    isConnected_ = true;
                }
                connectCv_.notify_all();

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

void AlpacaDataFeed::parseTradeUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaDataFeed] TradeUpdate Received: {}", data);
    try {
        MarketTick tick;
        tick.type = TickType::TRADE;
        tick.symbol = data.at("S").get<std::string>();
        tick.price = data.at("p").get<double>();
        tick.volume = data.at("s").get<double>();
        
        // Parse the timestamp string to epoch milliseconds
        std::string tsString = data.value("t", "");
        tick.timestamp_ms = parseAlpacaTimestamp(tsString);

        if (eventBus_) {
            eventBus_->publishTick(tick);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaDataFeed] Error parsing trade: {}", e.what());
    }
}

void AlpacaDataFeed::parseQuoteUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaDataFeed] QuoteUpdate Received: {}", data);
    try {
        MarketTick tick;
        tick.type = TickType::QUOTE;
        tick.symbol = data.at("S").get<std::string>();
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
        LOG_ERROR("[AlpacaDataFeed] Error parsing quote: {}", e.what());
    }
}

void AlpacaDataFeed::parseOrderBookUpdate(const nlohmann::json& data) {
    // LOG_DEBUG("[AlpacaDataFeed] BookUpdate Received: {}", data);
    try {
        MarketTick tick;
        tick.type = TickType::ORDERBOOK;
        tick.symbol = data.at("S").get<std::string>();
        
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
        LOG_ERROR("[AlpacaDataFeed] Error parsing orderbook: {}", e.what());
    }
}

} // namespace bot