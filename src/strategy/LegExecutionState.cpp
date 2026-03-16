#include <strategy/LegExecutionState.hpp>

namespace bot {

LegExecutionState::LegExecutionState(std::string symbol, std::string clientOrderId, double targetQty, Side side)
    : symbol_(std::move(symbol)), client_order_id_(std::move(clientOrderId)), 
      target_qty_(targetQty), side_(side) {}

bool LegExecutionState::updateFromReport(const ExecutionReport& report) {
    if (report.client_order_id != client_order_id_) return false;

    current_status_ = report.status;
    filled_qty_ = report.cumulative_quantity;

    // Return true if this update pushed the leg into a terminal state
    return isTerminal();
}

bool LegExecutionState::isTerminal() const {
    return current_status_ == OrderStatus::FILLED || 
           current_status_ == OrderStatus::CANCELED || 
           current_status_ == OrderStatus::REJECTED ||
           current_status_ == OrderStatus::EXPIRED;
}

double LegExecutionState::getFilledQuantity() const { return filled_qty_; }
double LegExecutionState::getTargetQuantity() const { return target_qty_; }
std::string LegExecutionState::getSymbol() const { return symbol_; }
std::string LegExecutionState::getClientOrderId() const { return client_order_id_; }
Side LegExecutionState::getSide() const { return side_; }

} // namespace bot