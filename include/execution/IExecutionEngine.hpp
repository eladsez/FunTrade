#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include <core/DomainModels.hpp>

// Interface for routing orders to the exchange.
class IExecutionEngine {
public:
    virtual ~IExecutionEngine() = default;

    // Authenticates with the exchange API.
    virtual bool authenticate() = 0;

    // Sends a new order request to the exchange.
    // Returns a unique order ID assigned by the exchange, or an empty string on failure.
    [[nodiscard]] virtual std::string placeOrder(const OrderRequest& request) = 0;

    // Cancels an active open order.
    virtual bool cancelOrder(const std::string& orderId) = 0;
    
    // Portfolio tracking
    virtual std::optional<double> getPortfolioValue() = 0;
    virtual nlohmann::json getOpenPositions() = 0;
};