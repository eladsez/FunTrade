#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <core/DomainModels.hpp>

namespace bot {

// Interface for routing orders to the exchange.
class IExecutionEngine {
public:
    virtual ~IExecutionEngine() = default;

    // Authenticates with the exchange API.
    virtual bool authenticate() = 0;

    // Sends a new order request to the exchange.
    // Returns a unique client-assigned order ID (client_order_id), or empty string if send failed.
    // This call is non-blocking (asynchronous in production implementation).
    [[nodiscard]] virtual std::string placeOrder(const OrderRequest& request) = 0;

    // Cancels an active open order.
    virtual bool cancelOrder(const std::string& orderId) = 0;
    
    // Portfolio tracking
    virtual std::optional<double> getPortfolioValue() = 0;
    virtual nlohmann::json getOpenPositions() = 0;
    virtual nlohmann::json getOpenOrders() = 0;
};

} // namespace bot