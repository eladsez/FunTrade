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
    std::vector<size_t> short_legs, long_legs;
    double short_weight_sum = 0.0, long_weight_sum = 0.0;

    for (size_t i = 0; i < num_components; ++i) {
        ratios[i] = std::abs(weights[i]) / total_abs_weight;
        if (weights[i] * direction < 0) {
            short_legs.push_back(i);
            short_weight_sum += ratios[i];
            targets[i].side = Side::SELL;
        } else if (weights[i] * direction > 0) {
            long_legs.push_back(i);
            long_weight_sum += ratios[i];
            targets[i].side = Side::BUY;
        }
    }

    if (short_legs.empty() || long_legs.empty()) return std::nullopt;

    // 1. Base Capital (Anchor on most restrictive short leg)
    double base_capital = 0.0;
    for (size_t i : short_legs) {
        if (currentPrices[i] <= 0.0) return std::nullopt; 
        double req = currentPrices[i] / ratios[i];
        if (req > base_capital) base_capital = req;
    }

    if (base_capital > capitalAllocation) return std::nullopt; 

    // 2. Dynamic Scaling
    int multiplier = static_cast<int>(capitalAllocation / base_capital);
    if (multiplier == 0) return std::nullopt;
    double target_cap = base_capital * multiplier;

    // 3. Short Shares (Strict Integers)
    double actual_short_cap = 0.0;
    for (size_t i : short_legs) {
        double ideal_cap = target_cap * ratios[i];
        double shares = std::round(ideal_cap / currentPrices[i]);
        if (shares < 1.0) return std::nullopt;
        
        targets[i].quantity = shares;
        actual_short_cap += (shares * currentPrices[i]);
    }

    // 4. Long Shares (Compensatory Fractional Rebalancing)
    double target_long_cap = actual_short_cap * (long_weight_sum / short_weight_sum);
    for (size_t i : long_legs) {
        double ideal_long_cap = target_long_cap * (ratios[i] / long_weight_sum);
        targets[i].quantity = ideal_long_cap / currentPrices[i];
    }

    // 5. Tracking Error Gatekeeper
    double total_actual_cap = actual_short_cap + target_long_cap;
    double max_error = 0.025; 
    for (size_t i = 0; i < num_components; ++i) {
        double actual_ratio = (targets[i].quantity * currentPrices[i]) / total_actual_cap;
        if (std::abs(actual_ratio - ratios[i]) / ratios[i] > max_error) {
            return std::nullopt; // Tracking error too high
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