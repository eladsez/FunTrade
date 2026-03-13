#include <strategy/BasketState.hpp>

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
}

// Fast O(N) linear search for small arrays (fits in CPU cache)
inline void BasketState::updatePrice(const std::string& symbol, double price, uint64_t timestamp) {
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
        spread += current_prices_[i] * weights_[i];
    }
    return spread;
}

// Call this only when a new bar closes (e.g., end of day/hour), NOT on every tick
void BasketState::advanceWindow() {
    if (!arePricesSynchronized()) return;

    double current_spread = calculateCurrentSpread();

    // O(1) Rolling Statistics Update
    double old_spread = spread_history_[history_idx_];
    spread_history_[history_idx_] = current_spread;

    running_sum_ += current_spread - old_spread;
    running_sum_sq_ += (current_spread * current_spread) - (old_spread * old_spread);

    history_idx_ = (history_idx_ + 1) % window_size_;
}

// Used during initial warm-up to feed historical spreads
void BasketState::feedHistoricalSpread(double spread) {
    double old_spread = spread_history_[history_idx_];
    spread_history_[history_idx_] = spread;

    running_sum_ += spread - old_spread;
    running_sum_sq_ += (spread * spread) - (old_spread * old_spread);

    history_idx_ = (history_idx_ + 1) % window_size_;
}

double BasketState::getZScore() const {
    if (!is_warmed_up_ || !arePricesSynchronized()) return 0.0;

    double current_spread = calculateCurrentSpread();
    double mean = running_sum_ / window_size_;
    
    // Guard against floating point imprecision
    double variance = std::max(0.0, (running_sum_sq_ - (running_sum_ * running_sum_) / window_size_) / (window_size_ - 1));
    double stddev = std::sqrt(variance);

    if (stddev < 1e-6) return 0.0;
    return (current_spread - mean) / stddev;
}

} // namespace bot