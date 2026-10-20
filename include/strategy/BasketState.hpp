#pragma once
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <optional>

namespace bot {

enum class PositionState {
    FLAT,
    LONG,
    SHORT,
};

// Price state of one basket. spread = sum_i w_i * log(price_i).
//
// The trading signal is daily and matches the research simulator (scripts/compute.py)
// exactly: for a trading day d with closes for every leg,
//     z(d) = (spread(d) - mean) / std      over the W most recent complete days ending at d
// (window includes d, sample std). Days are keyed by tradingDay(), so a close re-sent for
// the same day overwrites it instead of advancing the window, and no z exists until W
// complete days are known.
//
// Live prices are tracked separately, for order sizing and an indicative intraday z (UI).
class BasketState {
public:
    BasketState(int window_size, const std::vector<std::string>& symbols, const std::vector<double>& weights, std::string basket_id);

    // --- Daily closes (the signal) ---
    void setDailyClose(const std::string& symbol, int64_t day, double close_price);
    bool isDayComplete(int64_t day) const;
    std::optional<int64_t> latestCompleteDay() const;
    size_t completeDayCount() const;
    std::vector<int64_t> completeDays() const;  // oldest first
    std::optional<double> dailyZScore(int64_t day) const;
    // Closes of `day` per leg, if the day is complete.
    std::optional<std::vector<double>> closesOn(int64_t day) const {
        if (!isDayComplete(day)) return std::nullopt;
        return closes_.at(day);
    }
    void clearDailyHistory() { closes_.clear(); }

    // --- Live prices (sizing, UI) ---
    void updatePrice(const std::string& symbol, double price, uint64_t timestamp);
    bool hasAllPrices() const;
    double getPrice(size_t index) const { return current_prices_[index]; }
    // Live spread against the latest W complete days. Informational only: never traded on.
    std::optional<double> indicativeZScore() const;

    const std::vector<std::string>& getSymbols() const { return symbols_; }
    const std::vector<double>& getWeights() const { return weights_; }
    size_t getNumComponents() const { return num_components_; }
    int getWindowSize() const { return window_size_; }

private:
    int indexOf(const std::string& symbol) const;
    bool complete(const std::vector<double>& closes) const;
    double spreadOf(const std::vector<double>& prices) const;
    // Spreads of the last W complete days ending at `day` (inclusive), oldest first.
    std::optional<std::vector<double>> window(int64_t day) const;

    std::string basket_id_;
    int window_size_;
    std::vector<std::string> symbols_;
    std::vector<double> weights_;
    size_t num_components_;

    std::map<int64_t, std::vector<double>> closes_;  // day -> close per leg (0 = missing)
    std::vector<double> current_prices_;
    std::vector<uint64_t> last_timestamps_;
};

} // namespace bot
