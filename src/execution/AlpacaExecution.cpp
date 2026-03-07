#include <execution/AlpacaExecutionEngine.hpp>
#include <iostream>

namespace bot {

AlpacaExecutionEngine::AlpacaExecutionEngine(std::string apiKey, std::string apiSecret, std::string baseUrl)
    : apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)), baseUrl_(std::move(baseUrl)) {}

bool AlpacaExecutionEngine::authenticate() {
    // For Alpaca REST API, authentication happens via HTTP headers on every request.
    // APCA-API-KEY-ID and APCA-API-SECRET-KEY
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

    // Alpaca returns HTTP 204 or 200 on success usually; here we check if network simulation responds.
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
    (void)payload; // Suppress unused parameter warning
    // Placeholder for actual HTTP client (e.g. cURL or Boost.Beast)
    // Normally we'd set headers:
    // "APCA-API-KEY-ID: " + apiKey_
    // "APCA-API-SECRET-KEY: " + apiSecret_
    
    // Simulate successful JSON response for `placeOrder` for testing purposes
    if (method == "POST" && endpoint == "/v2/orders") {
        return R"({"id": "alpaca-mock-id-9999", "status": "accepted"})";
    } else if (method == "DELETE") {
        return R"({})"; // Simulate success
    } else if (method == "GET" && endpoint == "/v2/account") {
        return R"({"portfolio_value": "100500.25"})";
    } else if (method == "GET" && endpoint == "/v2/positions") {
        return R"([{"symbol": "BTC/USD", "qty": "1.5"}])";
    }
    
    return "";
}

} // namespace bot
