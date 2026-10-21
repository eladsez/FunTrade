#include <strategy/BasketState.hpp>
#include <cmath>
#include <numeric>
#include <core/Logger.hpp>
#include <risk/UiBroadcaster.hpp>

namespace bot {

namespace {
// Keep a few windows of history; older days can never be part of a z-score again.
constexpr size_t kHistoryWindows = 3;
}

BasketState::BasketState(int window_size, const std::vector<std::string>& symbols, const std::vector<double>& weights, std::string basket_id)
    : basket_id_(std::move(basket_id)), window_size_(window_size), symbols_(symbols), weights_(weights),
      num_components_(symbols.size()) {
    if (symbols_.size() != weights_.size()) {
        throw std::invalid_argument("Symbols and weights size mismatch");
    }
    if (window_size_ < 2) {
        throw std::invalid_argument("window_size must be at least 2");
    }
    current_prices_.resize(num_components_, 0.0);
    last_timestamps_.resize(num_components_, 0);
}

int BasketState::indexOf(const std::string& symbol) const {
    for (size_t i = 0; i < num_components_; ++i) {
        if (symbols_[i] == symbol) return static_cast<int>(i);
    }
    return -1;
}

bool BasketState::complete(const std::vector<double>& closes) const {
    return std::all_of(closes.begin(), closes.end(), [](double c) { return c > 0.0; });
}

double BasketState::spreadOf(const std::vector<double>& prices) const {
    double spread = 0.0;
    for (size_t i = 0; i < num_components_; ++i) {
        spread += std::log(prices[i]) * weights_[i];
    }
    return spread;
}

void BasketState::setDailyClose(const std::string& symbol, int64_t day, double close_price) {
    int i = indexOf(symbol);
    if (i < 0 || close_price <= 0.0) return;

    auto& row = closes_[day];
    if (row.empty()) row.resize(num_components_, 0.0);
    row[i] = close_price;

    size_t max_days = kHistoryWindows * static_cast<size_t>(window_size_);
    while (closes_.size() > max_days) closes_.erase(closes_.begin());
}

bool BasketState::isDayComplete(int64_t day) const {
    auto it = closes_.find(day);
    return it != closes_.end() && complete(it->second);
}

std::optional<int64_t> BasketState::latestCompleteDay() const {
    for (auto it = closes_.rbegin(); it != closes_.rend(); ++it) {
        if (complete(it->second)) return it->first;
    }
    return std::nullopt;
}

size_t BasketState::completeDayCount() const {
    return std::count_if(closes_.begin(), closes_.end(), [this](const auto& kv) { return complete(kv.second); });
}

std::vector<int64_t> BasketState::completeDays() const {
    std::vector<int64_t> out;
    for (const auto& [day, closes] : closes_) {
        if (complete(closes)) out.push_back(day);
    }
    return out;
}

std::optional<std::vector<double>> BasketState::window(int64_t day) const {
    if (!isDayComplete(day)) return std::nullopt;
    std::vector<double> spreads;
    for (auto it = std::make_reverse_iterator(closes_.upper_bound(day));
         it != closes_.rend() && spreads.size() < static_cast<size_t>(window_size_); ++it) {
        if (complete(it->second)) spreads.push_back(spreadOf(it->second));
    }
    if (spreads.size() < static_cast<size_t>(window_size_)) return std::nullopt;
    std::reverse(spreads.begin(), spreads.end());
    return spreads;
}

static std::optional<double> zOf(double value, const std::vector<double>& w) {
    double mean = std::accumulate(w.begin(), w.end(), 0.0) / w.size();
    double ss = 0.0;
    for (double x : w) ss += (x - mean) * (x - mean);
    double stddev = std::sqrt(ss / (w.size() - 1));  // sample std, as pandas rolling().std()
    if (!(stddev > 1e-12)) return std::nullopt;       // flat spread: no signal
    return (value - mean) / stddev;
}

std::optional<double> BasketState::dailyZScore(int64_t day) const {
    auto w = window(day);
    if (!w) return std::nullopt;
    return zOf(w->back(), *w);
}

void BasketState::updatePrice(const std::string& symbol, double price, uint64_t timestamp) {
    int i = indexOf(symbol);
    if (i < 0 || price <= 0.0) return;
    current_prices_[i] = price;
    last_timestamps_[i] = timestamp;
    UiBroadcaster::getInstance().broadcastPrice(basket_id_, symbol, price);
}

void BasketState::seedPrice(const std::string& symbol, double price) {
    int i = indexOf(symbol);
    if (i < 0 || price <= 0.0) return;
    current_prices_[i] = price;
    last_timestamps_[i] = 0;
}

bool BasketState::hasAllPrices() const {
    return complete(current_prices_);
}

std::optional<double> BasketState::indicativeZScore() const {
    if (!hasAllPrices()) return std::nullopt;
    auto day = latestCompleteDay();
    if (!day) return std::nullopt;
    auto w = window(*day);
    if (!w) return std::nullopt;
    return zOf(spreadOf(current_prices_), *w);
}

} // namespace bot
