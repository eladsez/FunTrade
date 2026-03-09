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
    AlpacaExecutionEngine(IEventBus& eventBus, std::shared_ptr<AlpacaInfra> infra, std::string apiKey, std::string apiSecret);
    ~AlpacaExecutionEngine() override;

    // IExecutionEngine Implementation
    bool connectToInfra() override;
    void disconnectFromInfra() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;

    // Extended Domain Specific API
    std::optional<double> getPortfolioValue() override;
    nlohmann::json getOpenPositions() override;
    nlohmann::json getOpenOrders() override;

private:
    IEventBus& eventBus_;
    std::shared_ptr<AlpacaInfra> infra_;
    std::string apiKey_;
    std::string apiSecret_;
    std::string baseHost_;

    void onWebSocketMessage(const std::string& payload);
};

} // namespace bot