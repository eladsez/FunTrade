#include <infra/AlpacaInfra.hpp>
#include <core/Logger.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <iostream>

#include <boost/beast/ssl.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace bot {

// The internal state holding Boost.Asio and Boost.Beast objects.
// This hides the heavy template logic from the header file.
struct InfraState {
    net::io_context ioc;
    net::executor_work_guard<net::io_context::executor_type> workGuard;
    ssl::context ctx{ssl::context::tlsv12_client};
    
    // WebSocket specific state
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
    beast::flat_buffer wsBuffer;
    std::deque<std::string> wsWriteQueue;
    bool isWsWriting = false;

    InfraState() : workGuard(net::make_work_guard(ioc)) {}
};

AlpacaInfra::AlpacaInfra(std::string apiKey, std::string apiSecret)
    : apiKey_(std::move(apiKey)), 
      apiSecret_(std::move(apiSecret)), 
      state_(std::make_unique<InfraState>()) {
    
    // Configure SSL context for generic client verification
    state_->ctx.set_default_verify_paths();
    state_->ctx.set_verify_mode(ssl::verify_peer);
}

AlpacaInfra::~AlpacaInfra() {
    disconnectWs();
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
}

// -----------------------------------------------------------------------------
// WebSocket API Implementation
// -----------------------------------------------------------------------------

void AlpacaInfra::setWsMessageCallback(std::function<void(const std::string&)> callback) {
    wsCallback_ = std::move(callback);
}

bool AlpacaInfra::connectWs(const std::string& host, const std::string& path, WsType type) {
    if (isWsConnected_) return true;

    currentWsType_ = type;
    isAuthenticating_ = true;
    authPromise_ = std::make_shared<std::promise<bool>>();
    auto future = authPromise_->get_future();

    try {
        tcp::resolver resolver(state_->ioc);
        state_->ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(state_->ioc), state_->ctx);

        auto const results = resolver.resolve(host, "443");
        
        // Use beast::get_lowest_layer
        beast::get_lowest_layer(*state_->ws).connect(results);

        // Set SNI Hostname (critical for modern TLS)
        if (!SSL_set_tlsext_host_name(state_->ws->next_layer().native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Perform SSL Handshake
        LOG_INFO("[AlpacaInfra] Performing SSL handshake...");
        state_->ws->next_layer().handshake(ssl::stream_base::client);
        LOG_INFO("[AlpacaInfra] SSL handshake successful.");
        
        // Perform WebSocket Handshake
        LOG_INFO("[AlpacaInfra] Performing WebSocket handshake with host={} path={}...", host, path);
        state_->ws->handshake(host, path);
        LOG_INFO("[AlpacaInfra] WebSocket handshake successful.");
        isWsConnected_ = true;

        // Start reading asynchronously
        doWsRead();

        // Launch the IO context run loop in a dedicated background thread
        ioThread_ = std::thread(&AlpacaInfra::runIoContext, this);

        if (currentWsType_ == WsType::EXECUTION) {
            nlohmann::json authPayload = {
                {"action", "auth"},
                {"key", apiKey_},
                {"secret", apiSecret_}
            };
            sendWsMessage(authPayload.dump());
        }
        
        // BLOCK the calling thread until auth succeeds or times out (5 seconds)
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
            return future.get();
        } else {
            LOG_ERROR("[AlpacaInfra] WebSocket Authentication Timeout.");
            disconnectWs();
            return false;
        }

    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaInfra] Connection Error: {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("[AlpacaInfra] Unknown Connection Error.");
        return false;
    }
}

void AlpacaInfra::runIoContext() {
    try {
        state_->ioc.run();
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaInfra] IO Context Error: {}", e.what());
    }
}

void AlpacaInfra::disconnectWs() {
    if (!isWsConnected_) return;

    if (state_->ws && state_->ws->is_open()) {
        // Dispatch the close command to the strand safely
        net::post(state_->ws->get_executor(), [this]() {
            state_->ws->async_close(websocket::close_code::normal, [](beast::error_code){});
        });
    }
    
    // Stop the io_context to unblock the background thread
    state_->ioc.stop();
    isWsConnected_ = false;
}

void AlpacaInfra::sendWsMessage(const std::string& message) {
    if (!isWsConnected_ || !state_->ws) {
        LOG_ERROR("[AlpacaInfra] Cannot send WS message: Not connected.");
        return;
    }

    // Post to the executor to ensure thread-safe writes
    net::post(state_->ws->get_executor(), [this, msg = message]() {
        state_->wsWriteQueue.push_back(msg);
        if (!state_->isWsWriting) {
            doWsWrite();
        }
    });
}

void AlpacaInfra::doWsWrite() {
    if (state_->wsWriteQueue.empty()) {
        state_->isWsWriting = false;
        return;
    }

    state_->isWsWriting = true;
    state_->ws->text(true);

    state_->ws->async_write(
        net::buffer(state_->wsWriteQueue.front()),
        [this](beast::error_code ec, std::size_t) {
            if (ec) {
                LOG_ERROR("[AlpacaInfra] WS Write Error: {}", ec.message());
                state_->isWsWriting = false;
                return;
            }
            state_->wsWriteQueue.pop_front();
            doWsWrite(); // Recursive call to empty the queue
        }
    );
}

void AlpacaInfra::doWsRead() {
    if (!isWsConnected_ || !state_->ws->is_open()) return;

    state_->ws->async_read(state_->wsBuffer, [this](beast::error_code ec, std::size_t) {
        if (ec) {
            LOG_ERROR("[AlpacaInfra] WS Read Error: {}", ec.message());
            isWsConnected_ = false;
            
            // CRITICAL: Unblock the main thread if the connection drops during auth
            if (isAuthenticating_) {
                isAuthenticating_ = false;
                authPromise_->set_value(false);
            }
            return;
        }

        std::string payload = beast::buffers_to_string(state_->wsBuffer.data());
        state_->wsBuffer.consume(state_->wsBuffer.size());

        if (isAuthenticating_) {
            handleAuthMessage(payload);
        } else if (wsCallback_) {
            // Pass pure market/execution data to the Domain layer
            wsCallback_(payload);
        }

        doWsRead();
    });
}

// The internal Alpaca handshake logic
void AlpacaInfra::handleAuthMessage(const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        
        if (currentWsType_ == WsType::DATA) {
            // NOTE: The Data API sends an ARRAY of JSON objects
            for (const auto& msg : j) {
                std::string msgType = msg.value("T", "");
                
                if (msgType == "success" && msg.value("msg", "") == "connected") {
                    nlohmann::json auth = {
                        {"action", "auth"}, 
                        {"key", apiKey_}, 
                        {"secret", apiSecret_}
                    };
                    sendWsMessage(auth.dump());
                } 
                else if (msgType == "success" && msg.value("msg", "") == "authenticated") {
                    LOG_INFO("[AlpacaInfra] Data Authentication SUCCESSFUL.");
                    isAuthenticating_ = false;
                    authPromise_->set_value(true);
                } 
                else if (msgType == "error") {
                    LOG_ERROR("[AlpacaInfra] Data Auth Failed: {}", msg.dump());
                    isAuthenticating_ = false;
                    authPromise_->set_value(false);
                }
            }
        } 
        else if (currentWsType_ == WsType::EXECUTION) {
            // NOTE: The Execution API sends a SINGLE JSON object
            std::string stream = j.value("stream", "");
            
            if (stream == "authorization") {
                auto data = j.value("data", nlohmann::json::object());
                std::string status = data.value("status", "");
                
                if (status == "unauthorized") {
                    // 1. Server connected, prompting for auth (in case we didn't send it immediately)
                    nlohmann::json authPayload = {
                        {"action", "auth"},
                        {"key", apiKey_},
                        {"secret", apiSecret_}
                    };
                    sendWsMessage(authPayload.dump());
                } 
                else if (status == "authorized") {
                    // 2. Auth successful!
                    LOG_INFO("[AlpacaInfra] Execution Authentication SUCCESSFUL.");
                    isAuthenticating_ = false;
                    authPromise_->set_value(true);
                }
            } 
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaInfra] JSON Parse Error during Auth: {} | Payload: {}", e.what(), payload);
        isAuthenticating_ = false;
        authPromise_->set_value(false);
    }
}

// -----------------------------------------------------------------------------
// REST API Implementation (Synchronous)
// -----------------------------------------------------------------------------

std::string AlpacaInfra::sendRestRequest(const std::string& method, const std::string& host, const std::string& target, const std::string& payload) {
    try {
        net::io_context restIoc;
        ssl::context restCtx(ssl::context::tlsv12_client);
        restCtx.set_default_verify_paths();
        restCtx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(restIoc);
        beast::ssl_stream<beast::tcp_stream> stream(restIoc, restCtx);

        if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        auto const results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::verb httpVerb = http::verb::get;
        if (method == "POST") httpVerb = http::verb::post;
        else if (method == "DELETE") httpVerb = http::verb::delete_;

        http::request<http::string_body> req{httpVerb, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set("APCA-API-KEY-ID", apiKey_);
        req.set("APCA-API-SECRET-KEY", apiSecret_);

        if ((httpVerb == http::verb::post || httpVerb == http::verb::put) && !payload.empty()) {
            req.set(http::field::content_type, "application/json");
            req.body() = payload;
            req.prepare_payload();
        }

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);
        return res.body();

    } catch (std::exception const& e) {
        LOG_ERROR("[AlpacaInfra] REST Request failed: {}", e.what());
        return "";
    }
}

} // namespace bot