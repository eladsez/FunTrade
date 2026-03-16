#include <strategy/ExecutionCycle.hpp>
#include <core/Logger.hpp>

namespace bot {

ExecutionCycle::ExecutionCycle(PositionState targetState) 
    : target_state_(targetState) {}

void ExecutionCycle::addLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side) {
    // If target is 0 (e.g., we are flattening a leg we don't hold), it's instantly terminal
    if (targetQty <= 0.0) {
        terminal_count_++;
        return; 
    }
    
    legs_.emplace(clientOrderId, LegExecutionState(symbol, clientOrderId, targetQty, side));
}

bool ExecutionCycle::processExecutionReport(const ExecutionReport& report) {
    auto it = legs_.find(report.client_order_id);
    if (it == legs_.end()) return false; // Not our order

    bool wasTerminal = it->second.isTerminal();
    bool isNowTerminal = it->second.updateFromReport(report);

    // Only increment if it transitioned to terminal on THIS specific update
    if (!wasTerminal && isNowTerminal) {
        terminal_count_++;
    }

    return isComplete();
}

bool ExecutionCycle::isComplete() const {
    // Note: total expected legs includes the ones that started with 0 target qty
    // because addLeg instantly increments terminal_count_ for them.
    // However, legs_.size() only tracks active orders.
    // The true completion check is if terminal_count_ equals the number of legs we attempted to process.
    // A safer way:
    for (const auto& [cid, leg] : legs_) {
        if (!leg.isTerminal()) return false;
    }
    return true;
}

bool ExecutionCycle::hasAnyFills() const {
    for (const auto& [cid, leg] : legs_) {
        if (leg.getFilledQuantity() > 0.0) return true;
    }
    return false;
}

PositionState ExecutionCycle::getTargetState() const {
    return target_state_;
}

} // namespace bot