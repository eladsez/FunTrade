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

AlpacaExecutionEngine::AlpacaExecutionEngine(std::string apiKey, std::string apiSecret)
    : apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)), 
      baseHost_("paper-api.alpaca.markets") {

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
    LOG_INFO("[AlpacaExecutionEngine] connecting to Infra...");
    
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

const char* AlpacaExecutionEngine::sideToString(Side side) {
    return side == Side::BUY ? "buy" : "sell";
}

const char* AlpacaExecutionEngine::orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::MARKET: return "market";
        case OrderType::LIMIT: return "limit";
        case OrderType::STOP: return "stop";
        case OrderType::STOP_LIMIT: return "stop_limit";
        case OrderType::TRAILING_STOP: return "trailing_stop";
        case OrderType::MARKET_ON_CLOSE: return "market";  // with time_in_force "cls"
        default: return "unknown";
    }
}

const char* AlpacaExecutionEngine::timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "gtc";
        case TimeInForce::DAY: return "day";
        case TimeInForce::IOC: return "ioc";
        case TimeInForce::FOK: return "fok";
        default: return "gtc"; 
    }
}

std::string AlpacaExecutionEngine::placeOrder(const OrderRequest& request) {

    // IExecutionEngine contract: send and report the request's id unchanged (unique per order).
    const std::string& clientOrderId = request.client_order_id;
    
    nlohmann::json orderPayload = {
        {"symbol", request.symbol},
        {"qty", formatDouble(request.quantity)},
        {"side", sideToString(request.side)},
        {"type", orderTypeToString(request.type)},
        {"time_in_force", request.type == OrderType::MARKET_ON_CLOSE ? "cls" : timeInForceToString(request.tif)},
        {"client_order_id", clientOrderId}
    };

    // Append pricing fields based on the specific order type
    switch (request.type) {
        case OrderType::LIMIT:
            orderPayload["limit_price"] = formatDouble(request.price);
            break;
        case OrderType::STOP:
            orderPayload["stop_price"] = formatDouble(request.stop_price);
            break;
        case OrderType::STOP_LIMIT:
            orderPayload["limit_price"] = formatDouble(request.price);
            orderPayload["stop_price"] = formatDouble(request.stop_price);
            break;
        case OrderType::TRAILING_STOP:
            if (request.trail_price > 0) {
                orderPayload["trail_price"] = formatDouble(request.trail_price);
            } else if (request.trail_percent > 0) {
                orderPayload["trail_percent"] = formatDouble(request.trail_percent);
            }
            break;
        case OrderType::MARKET:
        default:
            break;
    }

    std::string payloadStr = orderPayload.dump();

    LOG_DEBUG("[AlpacaExecutionEngine] Async Placing Order: {} | ClientID: {}", payloadStr, clientOrderId);

    // Detach a thread to perform the HTTP request without blocking the Strategy
    std::thread([this, payloadStr, clientOrderId, symbol = request.symbol, side = request.side]() {
        // An HTTP-level rejection (buying power, market closed, ...) never shows up on the
        // trade_updates stream, so report it here or the order would look pending forever.
        auto reportRejected = [&](const std::string& why) {
            LOG_ERROR("[AlpacaExecutionEngine] Order {} rejected: {}", clientOrderId, why);
            ExecutionReport r;
            r.client_order_id = clientOrderId;
            r.symbol = symbol;
            r.side = side;
            r.status = OrderStatus::REJECTED;
            r.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (reportCb_) reportCb_(r);
        };

        std::string response = infra_->sendRestRequest("POST", baseHost_, "/v2/orders", payloadStr);
        
        if (response.empty()) {
            reportRejected("no response from REST API");
            return;
        }
        try {
            auto res = nlohmann::json::parse(response);
            if (res.contains("id")) {
                LOG_DEBUG("[AlpacaExecutionEngine] Order Accepted: ExchangeID={}, ClientID={}", res["id"].get<std::string>(), clientOrderId);
            } else {
                reportRejected(res.dump());
            }
        } catch (const std::exception& e) {
            reportRejected(std::string("unparseable response: ") + e.what());
        }
    }).detach();

    return clientOrderId;
}

bool AlpacaExecutionEngine::cancelOrder(const std::string& orderId) {
    std::thread([this, orderId]() {
        std::string response = infra_->sendRestRequest("DELETE", baseHost_, "/v2/orders/" + orderId);
        LOG_DEBUG("[AlpacaExecutionEngine] Cancel request sent for order: {}", orderId);
    }).detach();
    return true;
}

bool AlpacaExecutionEngine::cancelAllOrders() {
    LOG_WARN("[AlpacaExecutionEngine] EMERGENCY: Canceling ALL open orders...");
    
    std::thread([this]() {
        // Alpaca's endpoint to cancel all open orders is a DELETE to /v2/orders
        std::string response = infra_->sendRestRequest("DELETE", baseHost_, "/v2/orders");
        
        if (response.empty()) {
            LOG_ERROR("[AlpacaExecutionEngine] Failed to send Cancel All request.");
        } else {
            LOG_DEBUG("[AlpacaExecutionEngine] Cancel All request sent. Exchange Response: {}", response);
        }
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

std::optional<double> AlpacaExecutionEngine::getAvailableCash() {
    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/account");
    if (responseStr.empty()) return std::nullopt;
    
    try {
        auto responseJson = nlohmann::json::parse(responseStr);
        // "buying_power" is the total dollar amount you can safely use to open new positions.
        // You could also use "cash" if you strictly only want unsettled/settled cash ignoring margin.
        if (responseJson.contains("buying_power")) {
            return std::stod(responseJson["buying_power"].get<std::string>());
        }
    } catch (...) {
        LOG_ERROR("[AlpacaExecutionEngine] Failed to parse account buying power.");
    }
    return std::nullopt;
}

std::vector<Position> AlpacaExecutionEngine::getOpenPositions() {
    std::vector<Position> ret;

    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/positions");
    if (responseStr.empty()) {
        LOG_ERROR("[AlpacaExecutionEngine] Failed to Rest-GET positions");
        return ret;
    }

    try {
        nlohmann::json positionsJson = nlohmann::json::parse(responseStr);
    
        if (!positionsJson.is_array()) {
            return ret;
        }

        // Pre-allocate the vector to prevent reallocations
        ret.reserve(positionsJson.size());

        for (const auto& posJson : positionsJson) {         
            // Construct the Position directly in the vector's memory
            ret.emplace_back(Position{
                posJson.value("symbol", ""),
                std::stod(posJson.value("qty", "0")),
                std::stod(posJson.value("avg_entry_price", "0")),
                std::stod(posJson.value("current_price", "0")),
                0.0 // realized_pnl default
            });
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaExecutionEngine] Error parsing synced position: {}", e.what());
    }
    
    // RVO will prevent this from being copied
    return ret; 
}

std::vector<OrderState> AlpacaExecutionEngine::getOpenOrders() {
    std::vector<OrderState> ret;

    std::string responseStr = infra_->sendRestRequest("GET", baseHost_, "/v2/orders");
    if (responseStr.empty()){
        LOG_ERROR("[AlpacaExecutionEngine] Failed to Rest-GET orders");
        return ret;
    } 

    try {
        nlohmann::json ordersJson = nlohmann::json::parse(responseStr);

        if (!ordersJson.is_array()) {
            return ret;
        }

        // Pre-allocate to prevent reallocation overhead
        ret.reserve(ordersJson.size());

        for (const auto& oJson : ordersJson) {
            // Resolve enums first so we can construct the struct in place
            std::string sideStr = oJson.value("side", "buy");
            Side side = (sideStr == "buy") ? Side::BUY : Side::SELL;
            
            std::string typeStr = oJson.value("type", "market");
            OrderType type = OrderType::MARKET;
            if (typeStr == "limit") type = OrderType::LIMIT;
            else if (typeStr == "trailing_stop") type = OrderType::TRAILING_STOP;
            
            std::string statusStr = oJson.value("status", "new");
            OrderStatus status = OrderStatus::NEW;
            if (statusStr == "filled") status = OrderStatus::FILLED;
            else if (statusStr == "partially_filled") status = OrderStatus::PARTIALLY_FILLED;
            else if (statusStr == "canceled") status = OrderStatus::CANCELED;

            std::string qtyStr = oJson["qty"].is_null() ? "0" : oJson.value("qty", "0");
            std::string priceStr = oJson["limit_price"].is_null() ? "0" : oJson.value("limit_price", "0");

            OrderState state;
            state.order_id = oJson.value("id", "");
            state.client_order_id = oJson["client_order_id"].is_null() ? "" : oJson.value("client_order_id", "");
            state.symbol = oJson.value("symbol", "");
            state.side = side;
            state.type = type;
            state.price = std::stod(priceStr);
            state.quantity = std::stod(qtyStr);
            state.filled_quantity = 0.0; // Initialize tracking fields
            state.avg_price = 0.0;
            state.status = status;

            ret.push_back(state);
        }
            
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaExecutionEngine] Error parsing synced order: {}", e.what());
    }

    return ret;
}

void AlpacaExecutionEngine::onWebSocketMessage(const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        std::string stream = j.value("stream", "");
        
        if (stream == "trade_updates") {
            auto data = j["data"];
            // LOG_DEBUG("[AlpacaExecutionEngine] Raw Trade Update: {}", data.dump());
            
            std::string event = data["event"].get<std::string>();
            auto order = data["order"];

            ExecutionReport report;
            report.order_id = order["id"].get<std::string>();
            report.symbol = order["symbol"].get<std::string>();
            
            std::string sideStr = order["side"].get<std::string>();
            report.side = (sideStr == "buy") ? Side::BUY : Side::SELL;

            std::string statusStr = event; 
            if (statusStr == "fill") report.status = OrderStatus::FILLED;
            else if (statusStr == "partial_fill") report.status = OrderStatus::PARTIALLY_FILLED;
            else if (statusStr == "canceled" || statusStr == "expired" || statusStr == "done_for_day") report.status = OrderStatus::CANCELED;
            else if (statusStr == "rejected") report.status = OrderStatus::REJECTED;
            else if (statusStr == "new" || statusStr == "accepted" || statusStr == "pending_new" || statusStr == "pending_cancel") report.status = OrderStatus::NEW;  // pending_cancel can still fill
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

            report.client_order_id = order.value("client_order_id", "");
            if (statusStr == "fill" || statusStr == "partial_fill") {
                report.exec_id = data.value("execution_id", "");
            } else {
                report.last_quantity = 0.0;  // status-only event; only fills carry quantity
            }

            LOG_DEBUG("[AlpacaExecutionEngine] Execution Report: Symbol={}, Event={}, Status={}, Qty={}, Price={}", 
                     report.symbol, event, static_cast<int>(report.status), report.last_quantity, report.last_price);
            
            if (reportCb_)
                reportCb_(report);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[AlpacaExecutionEngine] JSON parsing error in WS message: {} | Payload: {}", e.what(), payload);
    } catch (...) {
        LOG_ERROR("[AlpacaExecutionEngine] Unknown error parsing WS message.");
    }
}

} // namespace bot