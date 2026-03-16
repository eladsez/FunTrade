#pragma once
#include <vector>
#include <string>
#include <optional>
#include <core/DomainModels.hpp>
#include <risk/PortfolioManager.hpp>
#include <strategy/BasketState.hpp>

namespace bot {

struct LegTarget {
    double quantity = 0.0;
    Side side = Side::BUY;
};

class BasketSizer {
public:
    // Calculates the complex fractional-long/integer-short entry math.
    // Returns std::nullopt if the tracking error or capital constraints fail.
    static std::optional<std::vector<LegTarget>> calculateEntry(
        PositionState targetState,
        double capitalAllocation,
        const std::vector<double>& weights,
        const std::vector<double>& currentPrices
    );

    // Calculates the exact opposite of our current inventory to flatten out.
    static std::vector<LegTarget> calculateExit(
        const std::vector<std::string>& clientOrderIds,
        const PortfolioManager& portfolioMngr
    );
};

} // namespace bot