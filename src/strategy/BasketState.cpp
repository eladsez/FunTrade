#include <strategy/BasketState.hpp>
#include <cmath>
#include <core/Logger.hpp>
#include <risk/UiBroadcaster.hpp>

namespace bot {


BasketState::BasketState(int window_size, const std::vector<std::string>& symbols, const std::vector<double>& weights, std::string basket_id)
    : basket_id_(basket_id), window_size_(window_size), symbols_(symbols), weights_(weights),
        num_components_(symbols.size()), history_idx_(0),
        running_sum_(0.0), is_warmed_up_(false),
        ema_periods_(10), // Define the period length here
        ema_multiplier_(2.0 / (ema_periods_ + 1.0)), // Calculate dynamically
        is_ema_initialized_(false),
        current_smoothed_spread_(0.0)
         {
        
    if (symbols_.size() != weights_.size()) {
        throw std::invalid_argument("Symbols and weights size mismatch");
    }

    spread_history_.resize(window_size_, 0.0);
    current_prices_.resize(num_components_, 0.0);
    last_timestamps_.resize(num_components_, 0);
    bar_closed_flags_.resize(num_components_, false);
    closing_prices_.resize(num_components_, 0.0);
}

// Fast O(N) linear search for small arrays (fits in CPU cache)
// Trigger the EMA update exactly ONCE per tick arrival
void BasketState::updatePrice(const std::string& symbol, double price, uint64_t timestamp) {
    if (price <= 0.0) return;

    UiBroadcaster::getInstance().broadcastPrice(basket_id_, symbol, price);

    for (size_t i = 0; i < num_components_; ++i) {
        if (symbols_[i] == symbol) {
            current_prices_[i] = price;
            last_timestamps_[i] = timestamp;
            
            /*
             * --- Update EMA here, immediately after recording the tick ---
             * It is better to keep updating the internal EMA even if one stock is slightly lagging,
             * but block the trading execution if the data is stale (in getZScore).
             * 
            */
            if (hasAllPrices()) {
                double raw_spread = calculateRawSpread();
                updateSpreadEma(raw_spread);
            }
            
            return;
        }
    }
}

bool BasketState::hasAllPrices() const {
    for (double price : current_prices_) {
        if (price == 0.0) return false;
    }
    return true;
}

// Calculates spread strictly using the EOD closing prices
double BasketState::calculateClosingSpread() const {
    double spread = 0.0;
    for (size_t i = 0; i < num_components_; ++i) {
        if (closing_prices_[i] > 0.0) {
            spread += std::log(closing_prices_[i]) * weights_[i];
        }
    }
    return spread;
}

// Feeds the barrier and advances the window using clean EOD data
void BasketState::updateDailyClose(const std::string& symbol, double close_price) {
    if (close_price <= 0.0) return;

    for (size_t i = 0; i < num_components_; ++i) {
        if (symbols_[i] == symbol) {
            closing_prices_[i] = close_price;
            bar_closed_flags_[i] = true;

            // If all components have reported their daily close, advance the math
            if (checkAndResetBarrier()) {
                LOG_INFO("[BasketState] All closing price of {} arrived, advancing window...", basket_id_);
                advanceWindow();
            }
            return;
        }
    }
}

bool BasketState::checkAndResetBarrier() {
    for (bool flag : bar_closed_flags_) {
        if (!flag) return false;
    }
    
    // Reset the barrier for the next trading day
    std::fill(bar_closed_flags_.begin(), bar_closed_flags_.end(), false);
    return true;
}

// relies entirely on the isolated closing_prices_
void BasketState::advanceWindow() {
    // guarantees all closing prices are present for today.

    double closing_spread = calculateClosingSpread();

    // O(1) Rolling Statistics Update using the clean EOD spread
    double old_spread = spread_history_[history_idx_];
    spread_history_[history_idx_] = closing_spread;

    running_sum_ += closing_spread - old_spread;

    history_idx_ = (history_idx_ + 1) % window_size_;
}

// Used during initial warm-up to feed historical spreads
void BasketState::feedHistoricalSpread(double spread) {
    double old_spread = spread_history_[history_idx_];
    spread_history_[history_idx_] = spread;

    running_sum_ += spread - old_spread;

    history_idx_ = (history_idx_ + 1) % window_size_;
}

// 1. Rename this or use it as your raw spread calculator
double BasketState::calculateRawSpread() const {
    double spread = 0.0;
    for (size_t i = 0; i < num_components_; ++i) {
        spread += std::log(current_prices_[i]) * weights_[i];
    }
    return spread;
}

// 2. The EMA Math
void BasketState::updateSpreadEma(double raw_spread) {
    if (!is_ema_initialized_) {
        current_smoothed_spread_ = raw_spread;
        is_ema_initialized_ = true;
    } else {
        current_smoothed_spread_ = ((raw_spread - current_smoothed_spread_) * ema_multiplier_) + current_smoothed_spread_;
    }
}

bool BasketState::arePricesSynchronized() const {
    if (!hasAllPrices()) return false;

    uint64_t min_ts = std::numeric_limits<uint64_t>::max();
    uint64_t max_ts = 0;

    for (uint64_t ts : last_timestamps_) {
        if (ts < min_ts) min_ts = ts;
        if (ts > max_ts) max_ts = ts;
    }

    // If the gap between the oldest and newest tick is greater than the threshold - they are out of sync
    return (max_ts - min_ts) <= max_allowed_staleness_ms_;
}

std::optional<double> BasketState::getZScore() const {
    if (!is_warmed_up_ || !arePricesSynchronized() || !is_ema_initialized_) {
        return std::nullopt;
    }

    // 1. Calculate the mean using the safe rolling sum
    double mean = running_sum_ / window_size_;
    
    // 2. Safely calculate variance via O(N) pass to prevent catastrophic cancellation
    double sum_sq_diff = 0.0;
    for (int i = 0; i < window_size_; ++i) {
        double diff = spread_history_[i] - mean;
        sum_sq_diff += diff * diff;
    }
    
    double variance = sum_sq_diff / (window_size_ - 1); // Bessel's correction for sample variance
    double stddev = std::sqrt(variance);

    if (stddev < 1e-6) return 0.0; // Prevent division by zero
    
    // Use the safely smoothed spread
    return (current_smoothed_spread_ - mean) / stddev;
}

} // namespace bot