#pragma once
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <strategy/LegExecutionState.hpp>
#include <strategy/BasketState.hpp>

namespace bot {

class ExecutionCycle {
public:
    explicit ExecutionCycle(std::string basket_id): basket_id_(std::move(basket_id)) { }

    void addLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side);
    // Restart: a leg whose fill so far (and whether it is still working) is already known.
    void restoreLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side,
                    double filledQty, OrderStatus status) {
        legs_.emplace(clientOrderId, LegExecutionState(symbol, clientOrderId, targetQty, side, basket_id_, filledQty, status));
    }
    
    // Processes the report. Returns true if the ENTIRE cycle is now complete.
    bool processExecutionReport(const ExecutionReport& report);
    
    // True once every leg is terminal. A cycle with no legs to trade (all targets 0) is complete.
    bool isComplete() const;
    bool hasAnyFills() const;
    bool hasLegs() const { return !legs_.empty(); }

    // Client order IDs of legs that are still working (not terminal).
    std::vector<std::string> workingClientOrderIds() const;
    
    // Calculates the total absolute difference between target and filled quantities across all legs
    double getMissedShares() const;

private:
    std::map<std::string, LegExecutionState> legs_;
    std::string basket_id_;
};

} // namespace bot