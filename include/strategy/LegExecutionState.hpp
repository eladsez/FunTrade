#pragma once
#include <string>
#include <core/DomainModels.hpp>

namespace bot {

class LegExecutionState {
public:
    LegExecutionState() = default;
    LegExecutionState(std::string symbol, std::string clientOrderId, double targetQty, Side side, std::string basket_id);
    // Restores a leg after a restart with what is already known about it.
    LegExecutionState(std::string symbol, std::string clientOrderId, double targetQty, Side side, std::string basket_id,
                      double filledQty, OrderStatus status)
        : LegExecutionState(std::move(symbol), std::move(clientOrderId), targetQty, side, std::move(basket_id)) {
        filled_qty_ = filledQty;
        current_status_ = status;
    }

    // Updates internal state and returns true if the status changed to terminal
    bool updateFromReport(const ExecutionReport& report);
    
    // Getters
    bool isTerminal() const;
    double getFilledQuantity() const;
    double getTargetQuantity() const;
    std::string getSymbol() const;
    std::string getClientOrderId() const;
    Side getSide() const;

private:
    std::string symbol_;
    std::string client_order_id_;
    std::string basket_id_;
    double target_qty_ = 0.0;
    double filled_qty_ = 0.0;
    Side side_;
    OrderStatus current_status_ = OrderStatus::NEW;
};

} // namespace bot