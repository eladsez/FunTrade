#include <execution/DummyExecution.hpp>
#include <core/Logger.hpp>

bool DummyExecution::authenticate() {
    LOG_INFO("[DummyExecution] Authenticated successfully.");
    return true;
}

std::string DummyExecution::placeOrder(const OrderRequest& request) {
    LOG_INFO("[DummyExecution] Order placed for {} qty: {} side: {}", request.symbol, request.quantity, (request.side == Side::BUY ? "BUY" : "SELL"));
    return "dummy_order_123";
}

bool DummyExecution::cancelOrder(const std::string& orderId) {
    LOG_INFO("[DummyExecution] Order cancelled: {}", orderId);
    return true;
}
