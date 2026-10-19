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