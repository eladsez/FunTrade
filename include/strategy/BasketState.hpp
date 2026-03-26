#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <optional>

namespace bot {

enum class PositionState { 
    FLAT, 
    PENDING_LONG, 
    LONG, 
    PENDING_SHORT, 
    SHORT, 
    PENDING_FLAT 
};

class BasketState {
public:
    BasketState(int window_size, const std::vector<std::string>& symbols, const std::vector<double>& weights);

    // Fast O(N) linear search for small arrays (fits in CPU cache)
    void updatePrice(const std::string& symbol, double price, uint64_t timestamp);

    // only when a new bar closes (e.g., end of day/hour), NOT on every tick
    void updateDailyClose(const std::string& symbol, double close_price);

    // Used during initial warm-up to feed historical spreads
    void feedHistoricalSpread(double spread);

    std::optional<double> getZScore() const;

    void setWarmedUp(bool state) { is_warmed_up_ = state; }
    bool isWarmedUp() const { return is_warmed_up_; }

    double getPrice(size_t index) const { return current_prices_[index]; }
    const std::vector<std::string>& getSymbols() const { return symbols_; }
    const std::vector<double>& getWeights() const { return weights_; }
    size_t getNumComponents() const { return num_components_; }
    int getWindowSize() const { return window_size_; }

private:
    bool arePricesSynchronized() const;
    bool hasAllPrices() const;
    double calculateClosingSpread() const;
    bool checkAndResetBarrier();
    void advanceWindow();
    double calculateRawSpread() const; 
    void updateSpreadEma(double raw_spread);

    int window_size_;
    std::vector<std::string> symbols_;
    std::vector<double> weights_;
    size_t num_components_;

    std::vector<double> closing_prices_;
    std::vector<bool> bar_closed_flags_; 

    std::vector<double> spread_history_;
    std::vector<double> current_prices_;
    std::vector<uint64_t> last_timestamps_; 
    uint64_t max_allowed_staleness_ms_ = 30000;
    size_t history_idx_;

    double running_sum_;
    double running_sum_sq_;
    bool is_warmed_up_;

    // --- Embedded EMA State ---
    int ema_periods_;
    double ema_multiplier_;
    bool is_ema_initialized_;
    double current_smoothed_spread_;
};

} // namespace bot