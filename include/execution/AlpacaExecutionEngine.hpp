#pragma once

#include <execution/IExecutionEngine.hpp>
#include <infra/AlpacaInfra.hpp>
#include <string>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include <core/IEventBus.hpp>

namespace bot {

class AlpacaExecutionEngine : public IExecutionEngine {
public:
    // We inject the shared Infra instance here
    AlpacaExecutionEngine(std::string apiKey, std::string apiSecret);
    ~AlpacaExecutionEngine() override;

    // IExecutionEngine Implementation
    bool connectToInfra() override;
    void disconnectFromInfra() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;
    bool cancelAllOrders() override;

    // Extended Domain Specific API
    std::optional<double> getPortfolioValue() override;
    std::optional<double> getAvailableCash() override;
    std::vector<Position> getOpenPositions() override;
    std::vector<OrderState> getOpenOrders() override;

private:
    std::shared_ptr<AlpacaInfra> infra_;
    std::string apiKey_;
    std::string apiSecret_;
    std::string baseHost_;

    const char* sideToString(Side side);
    const char* orderTypeToString(OrderType type);
    const char* timeInForceToString(TimeInForce tif);

    void onWebSocketMessage(const std::string& payload);
};

} // namespace bot