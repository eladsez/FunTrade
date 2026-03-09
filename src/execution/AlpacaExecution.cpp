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

AlpacaExecutionEngine::AlpacaExecutionEngine(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret)
    : eventBus_(eventBus), apiKey_(std::move(apiKey)), 
      apiSecret_(std::move(apiSecret)), baseHost_("paper-api.alpaca.markets") {

    infra_ = std::make_unique<AlpacaInfra>(apiKey_, apiSecret_);
    
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
        {"type", request.type == OrderType::MARKET ? "market" :
                (request.type == OrderType::LIMIT ? "limit" 
                : "trailing_stop")},
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

            std::string statusStr = event; 
            if (statusStr == "fill") report.status = OrderStatus::FILLED;
            else if (statusStr == "partial_fill") report.status = OrderStatus::PARTIALLY_FILLED;
            else if (statusStr == "canceled") report.status = OrderStatus::CANCELED;
            else if (statusStr == "rejected") report.status = OrderStatus::REJECTED;
            else if (statusStr == "new" || statusStr == "accepted") report.status = OrderStatus::NEW;
            else return; // Ignore other events for now

            // Helper lambda to safely parse floats/doubles that might be null or strings
            auto safeStod = [](const nlohmann::json& j_obj, const std::string& key) -> double {
                if (j_obj.contains(key) && !j_obj[key].is_null()) {
                    if (j_obj[key].is_string()) return std::stod(j_obj[key].get<std::string>());
                    if (j_obj[key].is_number()) return j_obj[key].get<double>();
                }
                return 0.0;
            };

            report.last_quantity = safeStod(data, "qty");
            report.last_price = safeStod(data, "price");
            report.cumulative_quantity = safeStod(order, "filled_qty");
            report.avg_price = safeStod(order, "filled_avg_price");
            
            report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            LOG_INFO("[AlpacaExecutionEngine] Execution Report: Symbol={}, Event={}, Status={}, Qty={}, Price={}", 
                     report.symbol, event, static_cast<int>(report.status), report.last_quantity, report.last_price);
            
            eventBus_->publishExecution(report);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaExecutionEngine] JSON parsing error in WS message: {} | Payload: {}", e.what(), payload);
    } catch (...) {
        LOG_ERROR("[AlpacaExecutionEngine] Unknown error parsing WS message.");
    }
}

} // namespace bot