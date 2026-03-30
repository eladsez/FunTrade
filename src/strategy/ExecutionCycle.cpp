#include <strategy/ExecutionCycle.hpp>
#include <core/Logger.hpp>

namespace bot {

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
    // If we haven't even registered legs yet, it's not complete
    if (legs_.empty()) return false;
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

double ExecutionCycle::getMissedShares() const {
    double missed_shares = 0.0;
    
    for (const auto& [cid, leg] : legs_) {
        double target = leg.getTargetQuantity();
        double filled = leg.getFilledQuantity();
        missed_shares += std::abs(target - filled);
    }
    
    return missed_shares;
}

bool ExecutionCycle::hasAnyFills() const {
    for (const auto& [cid, leg] : legs_) {
        if (leg.getFilledQuantity() > 0.0) return true;
    }
    return false;
}

} // namespace bot