#include <strategy/ExecutionCycle.hpp>
#include <core/Logger.hpp>

namespace bot {

void ExecutionCycle::addLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side) {
    // A zero target (e.g. flattening a leg we don't hold) needs no order, so it isn't tracked.
    if (targetQty <= 0.0) {
        return;
    }
    
    legs_.emplace(clientOrderId, LegExecutionState(symbol, clientOrderId, targetQty, side, basket_id_));
}

bool ExecutionCycle::processExecutionReport(const ExecutionReport& report) {
    auto it = legs_.find(report.client_order_id);
    if (it == legs_.end()) return false; // Not our order

    it->second.updateFromReport(report);
    return isComplete();
}

bool ExecutionCycle::isComplete() const {
    for (const auto& [cid, leg] : legs_) {
        if (!leg.isTerminal()) return false;
    }
    return true;
}

std::vector<std::string> ExecutionCycle::workingClientOrderIds() const {
    std::vector<std::string> ids;
    for (const auto& [cid, leg] : legs_) {
        if (!leg.isTerminal()) ids.push_back(cid);
    }
    return ids;
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