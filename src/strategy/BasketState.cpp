#include <strategy/BasketState.hpp>
#include <cmath>

namespace bot {


BasketState::BasketState(int window_size, const std::vector<std::string>& symbols, const std::vector<double>& weights)
    : window_size_(window_size), symbols_(symbols), weights_(weights),
        num_components_(symbols.size()), history_idx_(0),
        running_sum_(0.0), running_sum_sq_(0.0), is_warmed_up_(false) {
        
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
void BasketState::updatePrice(const std::string& symbol, double price, uint64_t timestamp) {
    if (price <= 0.0) return;

    for (size_t i = 0; i < num_components_; ++i) {
        if (symbols_[i] == symbol) {
            current_prices_[i] = price;
            last_timestamps_[i] = timestamp;
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
    running_sum_sq_ += (closing_spread * closing_spread) - (old_spread * old_spread);

    history_idx_ = (history_idx_ + 1) % window_size_;
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

double BasketState::calculateCurrentSpread() const {
    double spread = 0.0;
    for (size_t i = 0; i < num_components_; ++i) {
        spread += std::log(current_prices_[i]) * weights_[i];
    }
    return spread;
}

// Used during initial warm-up to feed historical spreads
void BasketState::feedHistoricalSpread(double spread) {
    double old_spread = spread_history_[history_idx_];
    spread_history_[history_idx_] = spread;

    running_sum_ += spread - old_spread;
    running_sum_sq_ += (spread * spread) - (old_spread * old_spread);

    history_idx_ = (history_idx_ + 1) % window_size_;
}

std::optional<double> BasketState::getZScore() const {
    if (!is_warmed_up_ || !arePricesSynchronized()) return std::nullopt;;

    double current_spread = calculateCurrentSpread();
    double mean = running_sum_ / window_size_;
    
    // Guard against floating point imprecision
    double variance = std::max(0.0, (running_sum_sq_ - (running_sum_ * running_sum_) / window_size_) / (window_size_ - 1));
    double stddev = std::sqrt(variance);

    if (stddev < 1e-6) return 0.0;
    return (current_spread - mean) / stddev;
}

} // namespace bot