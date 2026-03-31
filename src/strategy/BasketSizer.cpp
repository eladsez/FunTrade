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

    std::vector<double> ratios(num_components);
    double max_capital_req = 0.0;

    // 1. Find the base capital required to buy at least 1 share of the most restrictive leg
    for (size_t i = 0; i < num_components; ++i) {
        if (currentPrices[i] <= 0.0) {
            LOG_ERROR("[BasketSizer] Invalid price {:.2f} for component {}", currentPrices[i], i);
            return std::nullopt; 
        }
        
        ratios[i] = std::abs(weights[i]) / total_abs_weight;
        double leg_capital_req = currentPrices[i] / ratios[i];
        
        if (leg_capital_req > max_capital_req) {
            max_capital_req = leg_capital_req;
        }
    }

    if (max_capital_req > capitalAllocation) {
        LOG_WARN("[BasketSizer] Base capital requirement ({:.2f}) exceeds allocation ({:.2f}).", max_capital_req, capitalAllocation);
        return std::nullopt;
    }

    // 2. Scale up to fit the allocated capital
    int multiplier = static_cast<int>(capitalAllocation / max_capital_req);
    double target_cap = max_capital_req * multiplier;
    double total_actual_cap = 0.0;

    // 3. Assign integer shares and calculate actual deployed capital
    for (size_t i = 0; i < num_components; ++i) {
        double ideal_leg_cap = target_cap * ratios[i];
        targets[i].quantity = std::round(ideal_leg_cap / currentPrices[i]);
        
        if (targets[i].quantity < 1.0) {
            LOG_WARN("[BasketSizer] Leg {} rounded to 0 shares.", i);
            return std::nullopt;
        }

        total_actual_cap += (targets[i].quantity * currentPrices[i]);
        targets[i].side = (weights[i] * direction < 0) ? Side::SELL : Side::BUY;
    }

    // 4. Strict Tracking Error Gatekeeper (2.5% threshold)
    // Ensures that the discrete integer shares haven't warped the target risk distribution.
    double max_error = 0.025;
    for (size_t i = 0; i < num_components; ++i) {
        double actual_ratio = (targets[i].quantity * currentPrices[i]) / total_actual_cap;
        double error = std::abs(actual_ratio - ratios[i]) / ratios[i];
        
        if (error > max_error) {
            LOG_DEBUG("[BasketSizer] Integer rounding tracking error ({:.2f}%) exceeds threshold for leg {}.", error * 100, i);
            return std::nullopt;
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