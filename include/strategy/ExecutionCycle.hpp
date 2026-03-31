#pragma once
#include <map>
#include <string>
#include <memory>
#include <strategy/LegExecutionState.hpp>
#include <strategy/BasketState.hpp>

namespace bot {

class ExecutionCycle {
public:
    explicit ExecutionCycle(std::string basket_id): basket_id_(std::move(basket_id)) { }

    void addLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side);
    
    // Processes the report. Returns true if the ENTIRE cycle is now complete.
    bool processExecutionReport(const ExecutionReport& report);
    
    bool isComplete() const;
    bool hasAnyFills() const;
    
    // Calculates the total absolute difference between target and filled quantities across all legs
    double getMissedShares() const;

private:
    std::map<std::string, LegExecutionState> legs_;
    size_t terminal_count_ = 0;
    std::string basket_id_;
};

} // namespace bot