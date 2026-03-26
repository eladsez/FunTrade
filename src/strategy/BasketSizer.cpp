#include <strategy/BasketSizer.hpp>
#include <cmath>
#include <core/Logger.hpp>

namespace bot {

std::optional<std::vector<LegTarget>> BasketSizer::calculateEntry(
    PositionState targetState, double capitalAllocation,
    const std::vector<double>& weights, const std::vector<double>& currentPrices) {
    
    size_t num_components = weights.size();
    std::vector<LegTarget> targets(num_components);
    
    int direction = (targetState == PositionState::LONG) ? 1 : -1;
    double total_abs_weight = 0.0;
    for (double w : weights) total_abs_weight += std::abs(w);

    for (size_t i = 0; i < num_components; ++i) {
        // Price defense: Prevent division by zero or corrupted data
        if (currentPrices[i] <= 0.0) {
            LOG_ERROR("[BasketSizer] Invalid price {} for component {}", currentPrices[i], i);
            return std::nullopt; 
        }

        // 1. Calculate the exact capital allocation for this specific leg
        double ratio = std::abs(weights[i]) / total_abs_weight;
        double leg_capital = capitalAllocation * ratio;

        // 2. Pure fractional share calculation
        targets[i].quantity = leg_capital / currentPrices[i];
        
        // 3. Determine Side
        if (weights[i] * direction < 0) {
            targets[i].side = Side::SELL;
        } else {
            targets[i].side = Side::BUY;
        }
    }

    return targets;
}

std::vector<LegTarget> BasketSizer::calculateExit(
    const std::vector<std::string>& clientOrderIds, const PortfolioManager& portfolioMngr) {
    
    std::vector<LegTarget> targets(clientOrderIds.size());
    for (size_t i = 0; i < clientOrderIds.size(); ++i) {
        double net_qty = portfolioMngr.getNetPosition(clientOrderIds[i]);
        if (std::abs(net_qty) >= 1e-9) {
            targets[i].quantity = std::abs(net_qty);
            targets[i].side = (net_qty > 0) ? Side::SELL : Side::BUY;
        }
    }
    return targets;
}

} // namespace bot