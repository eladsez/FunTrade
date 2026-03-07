#pragma once
#include <execution/IExecutionEngine.hpp>

class DummyExecution : public IExecutionEngine {
public:
    bool authenticate() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;
};
