#include <execution/AlpacaExecutionEngine.hpp>
#include <core/Logger.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

#include <iomanip>
#include <sstream>
#include <chrono>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace bot {

struct AlpacaWsState {
    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws;
};

static std::string formatDouble(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(9) << value;
    std::string str = out.str();
    str.erase(str.find_last_not_of('0') + 1, std::string::npos);
    if (str.back() == '.') str.pop_back();
    return str;
}

AlpacaExecutionEngine::AlpacaExecutionEngine(IEventBus& eventBus, std::string apiKey, std::string apiSecret, std::string baseUrl)
    : eventBus_(eventBus), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)), baseUrl_(std::move(baseUrl)),
      workGuard_(net::make_work_guard(ioc_)) {
    
    ioThread_ = std::thread([this]() {
        ioc_.run();
    });
}

AlpacaExecutionEngine::~AlpacaExecutionEngine() {
    isWsRunning_ = false;
    if (wsState_ && wsState_->ws) {
        beast::error_code ec;
        beast::get_lowest_layer(*wsState_->ws).cancel(ec);
        beast::get_lowest_layer(*wsState_->ws).close(ec);
    }
    if (wsThread_.joinable()) wsThread_.join();

    workGuard_.reset();
    ioc_.stop();
    if (ioThread_.joinable()) ioThread_.join();
}

bool AlpacaExecutionEngine::authenticate() {
    LOG_INFO("[AlpacaExecutionEngine] Authentication credentials loaded.");
    
    // Start WebSocket for trade updates
    isWsRunning_ = true;
    wsThread_ = std::thread(&AlpacaExecutionEngine::runWsLoop, this);
    
    return true;
}

std::string AlpacaExecutionEngine::placeOrder(const OrderRequest& request) {
    std::string clientOrderId = request.client_order_id;
    if (clientOrderId.empty()) {
        clientOrderId = "bot_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }

    nlohmann::json orderPayload = {
        {"symbol", request.symbol},
        {"qty", formatDouble(request.quantity)},
        {"side", request.side == Side::BUY ? "buy" : "sell"},
        {"type", request.type == OrderType::MARKET ? "market" : (request.type == OrderType::LIMIT ? "limit" : "trailing_stop")},
        {"time_in_force", "gtc"},
        {"client_order_id", clientOrderId}
    };

    if (request.type == OrderType::LIMIT) {
        orderPayload["limit_price"] = formatDouble(request.price);
    } else if (request.type == OrderType::TRAILING_STOP) {
        if (request.trail_price > 0) orderPayload["trail_price"] = formatDouble(request.trail_price);
        else if (request.trail_percent > 0) orderPayload["trail_percent"] = formatDouble(request.trail_percent);
    }

    LOG_INFO("[AlpacaExecutionEngine] Async Placing Order: {} | ClientID: {}", orderPayload.dump(), clientOrderId);

    sendHttpRequestAsync("POST", "/v2/orders", orderPayload.dump(), [this, clientOrderId](const std::string& response) {
        if (response.empty()) {
            LOG_ERROR("[AlpacaExecutionEngine] Async placeOrder failed for ClientID: {}", clientOrderId);
            return;
        }
        try {
            auto res = nlohmann::json::parse(response);
            if (res.contains("id")) {
                LOG_INFO("[AlpacaExecutionEngine] Order Accepted: ExchangeID={}, ClientID={}", res["id"].get<std::string>(), clientOrderId);
            } else {
                LOG_ERROR("[AlpacaExecutionEngine] Order Rejected: {}", res.dump());
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[AlpacaExecutionEngine] JSON Error on Async response: {}", e.what());
        }
    });

    return clientOrderId;
}

bool AlpacaExecutionEngine::cancelOrder(const std::string& orderId) {
    // For now, cancel remains blocking or we can make it async too. 
    // Usually, strategies might want to wait for cancel confirmation, but non-blocking is better.
    sendHttpRequestAsync("DELETE", "/v2/orders/" + orderId, "", [orderId](const std::string& response) {
        LOG_INFO("[AlpacaExecutionEngine] Cancel request sent for order: {}", orderId);
    });
    return true;
}

std::optional<double> AlpacaExecutionEngine::getPortfolioValue() {
    std::string responseStr = sendHttpRequest("GET", "/v2/account");
    if (responseStr.empty()) return std::nullopt;
    try {
        auto responseJson = nlohmann::json::parse(responseStr);
        if (responseJson.contains("portfolio_value")) {
            return std::stod(responseJson["portfolio_value"].get<std::string>());
        }
    } catch (...) {}
    return std::nullopt;
}

nlohmann::json AlpacaExecutionEngine::getOpenPositions() {
    std::string responseStr = sendHttpRequest("GET", "/v2/positions");
    if (responseStr.empty()) return nlohmann::json::array();
    try {
        return nlohmann::json::parse(responseStr);
    } catch (...) {
        return nlohmann::json::array();
    }
}

nlohmann::json AlpacaExecutionEngine::getOpenOrders() {
    std::string responseStr = sendHttpRequest("GET", "/v2/orders");
    if (responseStr.empty()) return nlohmann::json::array();
    try {
        return nlohmann::json::parse(responseStr);
    } catch (...) {
        return nlohmann::json::array();
    }
}

void AlpacaExecutionEngine::sendHttpRequestAsync(const std::string& method, const std::string& endpoint, const std::string& payload, 
                                                std::function<void(const std::string&)> callback) {
    // Offload the synchronous HTTP call to the IO thread
    net::post(ioc_, [this, method, endpoint, payload, callback]() {
        std::string result = sendHttpRequest(method, endpoint, payload);
        callback(result);
    });
}

std::string AlpacaExecutionEngine::sendHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload) {
    try {
        std::string host = baseUrl_;
        if (host.find("https://") == 0) host = host.substr(8);
        else if (host.find("http://") == 0) host = host.substr(7);
        size_t slashPos = host.find('/');
        if (slashPos != std::string::npos) host = host.substr(0, slashPos);

        std::string port = "443";
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            return "";
        }

        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::verb verb = (method == "POST") ? http::verb::post : ((method == "DELETE") ? http::verb::delete_ : http::verb::get);
        http::request<http::string_body> req{verb, endpoint, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set("APCA-API-KEY-ID", apiKey_);
        req.set("APCA-API-SECRET-KEY", apiSecret_);

        if (!payload.empty()) {
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
    } catch(std::exception const& e) {
        LOG_ERROR("[AlpacaExecutionEngine] HTTP Error: {}", e.what());
        return "";
    }
}

void AlpacaExecutionEngine::runWsLoop() {
    wsState_ = std::make_unique<AlpacaWsState>();
    wsState_->ctx.set_default_verify_paths();
    
    std::string host = "paper-api.alpaca.markets"; // Using paper-api for stream
    std::string port = "443";
    
    while (isWsRunning_) {
        try {
            tcp::resolver resolver(wsState_->ioc);
            wsState_->ws = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(net::make_strand(wsState_->ioc), wsState_->ctx);

            auto const results = resolver.resolve(host, port);
            net::connect(beast::get_lowest_layer(*wsState_->ws), results);

            if(! SSL_set_tlsext_host_name(wsState_->ws->next_layer().native_handle(), host.c_str())) throw std::runtime_error("SNI failed");

            wsState_->ws->next_layer().handshake(ssl::stream_base::client);
            wsState_->ws->handshake(host, "/v2/stream");

            // Authenticate WS
            nlohmann::json auth = {{"action", "authenticate"}, {"data", {{"key_id", apiKey_}, {"secret_key", apiSecret_}}}};
            wsState_->ws->write(net::buffer(auth.dump()));

            // Subscribe to trade updates
            nlohmann::json sub = {{"action", "listen"}, {"data", {{"streams", {"trade_updates"}}}}};
            wsState_->ws->write(net::buffer(sub.dump()));

            beast::flat_buffer buffer;
            while (isWsRunning_ && wsState_->ws->is_open()) {
                wsState_->ws->read(buffer);
                onWsMessage(beast::buffers_to_string(buffer.data()));
                buffer.consume(buffer.size());
            }
        } catch (const std::exception& e) {
            if (isWsRunning_) {
                LOG_ERROR("[AlpacaExecutionEngine] WS Error: {}. Reconnecting in 5s...", e.what());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    }
}

void AlpacaExecutionEngine::onWsMessage(const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        std::string stream = j.value("stream", "");
        if (stream == "trade_updates") {
            auto data = j["data"];
            LOG_DEBUG("[AlpacaExecutionEngine] Raw Trade Update: {}", data.dump());
            std::string event = data["event"].get<std::string>();
            auto order = data["order"];

            ExecutionReport report;
            report.order_id = order["id"].get<std::string>();
            report.client_order_id = order.value("client_order_id", "");
            report.symbol = order["symbol"].get<std::string>();
            
            std::string sideStr = order["side"].get<std::string>();
            report.side = (sideStr == "buy") ? Side::BUY : Side::SELL;

            std::string statusStr = data["event"].get<std::string>(); // 'fill', 'partial_fill', etc.
            if (statusStr == "fill") report.status = OrderStatus::FILLED;
            else if (statusStr == "partial_fill") report.status = OrderStatus::PARTIALLY_FILLED;
            else if (statusStr == "canceled") report.status = OrderStatus::CANCELED;
            else if (statusStr == "rejected") report.status = OrderStatus::REJECTED;
            else if (statusStr == "new") report.status = OrderStatus::NEW;
            else return; // Ignore other events for now

            report.last_quantity = std::stod(data.value("qty", "0"));
            report.last_price = std::stod(data.value("price", "0"));
            report.cumulative_quantity = std::stod(order.value("filled_qty", "0"));
            report.avg_price = std::stod(order.value("filled_avg_price", "0"));
            report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            LOG_INFO("[AlpacaExecutionEngine] Execution Report: Symbol={}, Event={}, Status={}, Qty={}, Price={}", 
                     report.symbol, event, statusStr, report.last_quantity, report.last_price);
            
            eventBus_.publishExecution(report);
        }
    } catch (...) {}
}

} // namespace bot
