#pragma once
#include <map>
#include <string>
#include <memory>
#include <strategy/LegExecutionState.hpp>
#include <strategy/BasketState.hpp>

namespace bot {

class ExecutionCycle {
public:
    explicit ExecutionCycle(PositionState targetState);

    void addLeg(const std::string& clientOrderId, const std::string& symbol, double targetQty, Side side);
    
    // Processes the report. Returns true if the ENTIRE cycle is now complete.
    bool processExecutionReport(const ExecutionReport& report);
    
    bool isComplete() const;
    bool hasAnyFills() const;
    PositionState getTargetState() const;

private:
    PositionState target_state_;
    std::map<std::string, LegExecutionState> legs_;
    size_t terminal_count_ = 0;
};

} // namespace bot