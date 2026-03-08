#include <execution/DummyExecution.hpp>
#include <core/Logger.hpp>

#include <chrono>

DummyExecution::DummyExecution(IEventBus& eventBus) : eventBus_(eventBus) {}

bool DummyExecution::authenticate() {
    LOG_INFO("[DummyExecution] Authenticated successfully.");
    return true;
}

std::string DummyExecution::placeOrder(const OrderRequest& request) {
    std::string orderId = "dummy_order_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    LOG_INFO("[DummyExecution] {} Order placed for {} qty: {} price: {} id: {}", 
             (request.type == OrderType::LIMIT ? "LIMIT" : "MARKET"),
             request.symbol, request.quantity, request.price, orderId);
    
    // Simulate immediate fill
    ExecutionReport report;
    report.order_id = orderId;
    report.symbol = request.symbol;
    report.side = request.side;
    report.status = OrderStatus::FILLED;
    report.last_quantity = request.quantity;
    report.cumulative_quantity = request.quantity;
    
    // For Dummy, if it's a market order without price, we assume 50000.0 as a placeholder
    // In a real backtest, the price should come from the current tick.
    report.last_price = (request.price > 0) ? request.price : 50000.0;
    report.avg_price = report.last_price;
    
    report.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    eventBus_.publishExecution(report);

    return orderId;
}

bool DummyExecution::cancelOrder(const std::string& orderId) {
    LOG_INFO("[DummyExecution] Order cancelled: {}", orderId);
    return true;
}
