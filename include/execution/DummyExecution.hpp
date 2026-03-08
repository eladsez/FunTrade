#pragma once
#include <execution/IExecutionEngine.hpp>

#include <core/IEventBus.hpp>

class DummyExecution : public IExecutionEngine {
public:
    DummyExecution(IEventBus& eventBus);
    bool authenticate() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;
    
    std::optional<double> getPortfolioValue() override { return 100000.0; }
    nlohmann::json getOpenPositions() override { return nlohmann::json::array(); }

private:
    IEventBus& eventBus_;
};
