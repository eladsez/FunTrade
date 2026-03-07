#include <execution/AlpacaExecutionEngine.hpp>
#include <iostream>

// Boost libraries for asynchronous I/O and HTTP
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace bot {

AlpacaExecutionEngine::AlpacaExecutionEngine(std::string apiKey, std::string apiSecret, std::string baseUrl)
    : apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)), baseUrl_(std::move(baseUrl)) {}

bool AlpacaExecutionEngine::authenticate() {
    // For Alpaca REST API, authentication happens via HTTP headers on every request.
    std::cout << "[AlpacaExecutionEngine] Authentication credentials loaded.\n";
    return true;
}

std::string AlpacaExecutionEngine::placeOrder(const OrderRequest& request) {
    std::lock_guard<std::mutex> lock(requestMutex_);

    nlohmann::json orderPayload = {
        {"symbol", request.symbol},
        {"qty", std::to_string(request.quantity)}, // Alpaca often expects string for exact decimal precision
        {"side", request.side == Side::BUY ? "buy" : "sell"},
        {"type", request.type == OrderType::MARKET ? "market" : "limit"},
        {"time_in_force", "gtc"} // Assume Good-Till-Canceled for simplicity
    };

    if (request.type == OrderType::LIMIT) {
        orderPayload["limit_price"] = std::to_string(request.price);
    }

    std::cout << "[AlpacaExecutionEngine] Placing order: " << orderPayload.dump() << "\n";
    
    std::string responseStr = sendHttpRequest("POST", "/v2/orders", orderPayload.dump());
    
    if (responseStr.empty()) return "";

    try {
        auto responseJson = nlohmann::json::parse(responseStr);
        if (responseJson.contains("id")) {
            return responseJson["id"].get<std::string>();
        }
    } catch (const std::exception& e) {
        std::cerr << "[AlpacaExecutionEngine] JSON Parse Error on placeOrder: " << e.what() << "\n";
    }

    return "";
}

bool AlpacaExecutionEngine::cancelOrder(const std::string& orderId) {
    std::lock_guard<std::mutex> lock(requestMutex_);

    std::string endpoint = "/v2/orders/" + orderId;
    std::string responseStr = sendHttpRequest("DELETE", endpoint);

    return !responseStr.empty(); 
}

std::optional<double> AlpacaExecutionEngine::getPortfolioValue() {
    std::lock_guard<std::mutex> lock(requestMutex_);
    
    std::string responseStr = sendHttpRequest("GET", "/v2/account");
    if (responseStr.empty()) return std::nullopt;

    try {
        auto responseJson = nlohmann::json::parse(responseStr);
        if (responseJson.contains("portfolio_value")) {
            return std::stod(responseJson["portfolio_value"].get<std::string>());
        }
    } catch (...) {
        // Log error
    }
    
    return std::nullopt;
}

nlohmann::json AlpacaExecutionEngine::getOpenPositions() {
    std::lock_guard<std::mutex> lock(requestMutex_);
    
    std::string responseStr = sendHttpRequest("GET", "/v2/positions");
    if (responseStr.empty()) return nlohmann::json::array();

    try {
        return nlohmann::json::parse(responseStr);
    } catch (...) {
        return nlohmann::json::array();
    }
}

std::string AlpacaExecutionEngine::sendHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload) {
    try {
        // Extract host from baseUrl_ (e.g., https://paper-api.alpaca.markets/v2 -> paper-api.alpaca.markets)
        std::string host = baseUrl_;
        if (host.find("https://") == 0) host = host.substr(8);
        else if (host.find("http://") == 0) host = host.substr(7);
        
        // Strip any trailing path or slash for the DNS resolver
        size_t slashPos = host.find('/');
        if (slashPos != std::string::npos) {
            host = host.substr(0, slashPos);
        }

        std::string port = "443"; 

        // The io_context is required for all I/O
        net::io_context ioc;

        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv12_client);

        // This holds the root certificate used for verification
        // For simplicity in PoC, we might use default verification or bypass it.
        // It's recommended to load system certs:
        ctx.set_default_verify_paths();

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(stream).connect(results);

        // Perform the SSL handshake
        stream.handshake(ssl::stream_base::client);

        // Set up an HTTP GET/POST request message
        http::verb verb = http::verb::get;
        if (method == "POST") verb = http::verb::post;
        else if (method == "DELETE") verb = http::verb::delete_;

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

        // Send the HTTP request to the remote host
        http::write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);

        // Gracefully close the stream
        beast::error_code ec;
        stream.shutdown(ec);
        if(ec == net::error::eof) {
            // Rationale:
            // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
            ec = {};
        }

        return res.body();

    } catch(std::exception const& e) {
        std::cerr << "[AlpacaExecutionEngine] HTTP Request Failed: " << e.what() << "\n";
        return "";
    }
}

} // namespace bot