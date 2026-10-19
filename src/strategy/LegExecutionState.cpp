#include <strategy/LegExecutionState.hpp>
#include <risk/UiBroadcaster.hpp>
#include <algorithm>

namespace bot {

LegExecutionState::LegExecutionState(std::string symbol, std::string clientOrderId, double targetQty, Side side, std::string basket_id)
    : symbol_(std::move(symbol)), client_order_id_(std::move(clientOrderId)), basket_id_(std::move(basket_id)),
      target_qty_(targetQty), side_(side){}

bool LegExecutionState::updateFromReport(const ExecutionReport& report) {
    if (report.client_order_id != client_order_id_) return false;

    // Filled quantity only ever grows: cancel/reject reports may carry 0 (or a stale value),
    // and reports can arrive out of order.
    filled_qty_ = std::max(filled_qty_, report.cumulative_quantity);

    // Once terminal, a leg stays terminal: late or duplicate reports must not reopen it.
    if (!isTerminal()) {
        current_status_ = report.status;
    }

    UiBroadcaster::getInstance().broadcastFill(basket_id_, symbol_, target_qty_, filled_qty_);

    // Return true if this update pushed the leg into a terminal state
    return isTerminal();
}

bool LegExecutionState::isTerminal() const {
    return bot::isTerminal(current_status_);
}

double LegExecutionState::getFilledQuantity() const { return filled_qty_; }
double LegExecutionState::getTargetQuantity() const { return target_qty_; }
std::string LegExecutionState::getSymbol() const { return symbol_; }
std::string LegExecutionState::getClientOrderId() const { return client_order_id_; }
Side LegExecutionState::getSide() const { return side_; }

} // namespace bot