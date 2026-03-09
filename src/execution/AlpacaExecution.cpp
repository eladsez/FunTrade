#include <execution/AlpacaExecutionEngine.hpp>
#include <core/Logger.hpp>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <thread>

namespace bot {

static std::string formatDouble(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(9) << value;
    std::string str = out.str();
    str.erase(str.find_last_not_of('0') + 1, std::string::npos);
    if (str.back() == '.') str.pop_back();
    return str;
}

AlpacaExecutionEngine::AlpacaExecutionEngine(IEventBus& eventBus, std::shared_ptr<AlpacaInfra> infra, 
                                             std::string apiKey, std::string apiSecret)
    : eventBus_(eventBus), infra_(std::move(infra)), apiKey_(std::move(apiKey)), 
      apiSecret_(std::move(apiSecret)), baseHost_("paper-api.alpaca.markets") {
    
    // Register the websocket callback for trade updates
    infra_->setWsMessageCallback([this](const std::string& msg) {
        onWebSocketMessage(msg);
    });

    if (!connectToInfra()){
        throw std::runtime_error("[AlpacaExecutionEngine] Failed connecting to Infra");
    }
}

AlpacaExecutionEngine::~AlpacaExecutionEngine() {
    disconnectFromInfra();
}


bool AlpacaExecutionEngine::connectToInfra() {
    LOG_INFO("[AlpacaExecutionEngine] Authenticating via Infra...");
    
    bool success = infra_->connectWs(baseHost_, "/v2/stream", WsType::EXECUTION);
    
    if (success) {
        nlohmann::json sub = {
            {"action", "listen"}, 
            {"data", {{"streams", {"trade_updates"}}}}
        };
        infra_->sendWsMessage(sub.dump());
    } else {
        LOG_ERROR("[AlpacaExecutionEngine] Failed to connect execution WS.");
    }
    
    return success;
}

void AlpacaExecutionEngine::disconnectFromInfra(){
    infra_->disconnectWs();
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

    std::string payloadStr = orderPayload.dump();

    LOG_INFO("[AlpacaExecutionEngine] Async Placing Order: {} | ClientID: {}", payloadStr, clientOrderId);

    // Detach a thread to perform the HTTP request without blocking the Strategy
    std::thread([this, payloadStr, clientOrderId]() {
        std::string response = infra_->sendRestRequest("POST", baseHost_, "/v2/orders", payloadStr);
        
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
    }).detach();

    return clientOrderId;
}

bool AlpacaExecutionEngine::cancelOrder(const std::string& orderId) {
    std::thread([this, orderId]() {
        std::string response = infra_->sendRestRequest("DELETE", baseHost_, "/v2/orders/" + orderId);
        LOG_INFO("[AlpacaExecutionEngine] Cancel request sent for order: {}", orderId);
    }).detach();
    return true;
}

std::optional<double> AlpacaExecutionEngine::getPortfolioValue() {
    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/account");
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
    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/positions");
    if (responseStr.empty()) return nlohmann::json::array();
    
    try {
        return nlohmann::json::parse(responseStr);
    } catch (...) {
        return nlohmann::json::array();
    }
}

nlohmann::json AlpacaExecutionEngine::getOpenOrders() {
    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/orders");
    if (responseStr.empty()) return nlohmann::json::array();
    try {
        return nlohmann::json::parse(responseStr);
    } catch (...) {
        return nlohmann::json::array();
    }
}

void AlpacaExecutionEngine::onWebSocketMessage(const std::string& payload) {
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