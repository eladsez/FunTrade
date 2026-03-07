#include <execution/DummyExecution.hpp>
#include <iostream>

bool DummyExecution::authenticate() {
    std::cout << "[DummyExecution] Authenticated successfully.\n";
    return true;
}

std::string DummyExecution::placeOrder(const OrderRequest& request) {
    std::cout << "[DummyExecution] Order placed for " << request.symbol << " qty: " << request.quantity << " side: " << (request.side == Side::BUY ? "BUY" : "SELL") << "\n";
    return "dummy_order_123";
}

bool DummyExecution::cancelOrder(const std::string& orderId) {
    std::cout << "[DummyExecution] Order cancelled: " << orderId << "\n";
    return true;
}
