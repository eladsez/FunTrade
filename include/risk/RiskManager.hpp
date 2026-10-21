#pragma once
#include <risk/PortfolioManager.hpp>
#include <strategy/BasketSizer.hpp>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bot {

struct RiskLimits {
    double max_gross_exposure_usd = 0.0;  // sum |qty * price| over all baskets (0 = no limit)
    double max_symbol_net_usd = 0.0;      // |net qty * price| per symbol across baskets (0 = no limit)
    double max_basket_loss_usd = 0.0;     // basket exits then halts below -limit (0 = no limit)
    // Operator controls: create the file to activate, delete it to clear.
    std::string kill_switch_file;         // no new entries while it exists
    std::string flatten_file;             // every basket exits at its next session while it exists
};

// Portfolio-level guard shared by all strategies. Entries reserve their exposure until the
// cycle resolves, so baskets that all submit at the same session open (MOC orders fill only
// at the close) cannot jointly exceed the limits. Thread-safe.
class RiskManager {
public:
    RiskManager(std::shared_ptr<PortfolioManager> pm, RiskLimits limits)
        : pm_(std::move(pm)), limits_(std::move(limits)) {}

    // nullopt if approved (and reserved under `basket_id`), otherwise the reason it was rejected.
    std::optional<std::string> approveEntry(const std::string& basket_id,
                                            const std::vector<std::string>& symbols,
                                            const std::vector<LegTarget>& targets,
                                            const std::vector<double>& prices) {
        if (killSwitchActive()) return "kill switch active (" + limits_.kill_switch_file + ")";

        std::lock_guard<std::mutex> lock(mtx_);
        auto r = reservationOf(symbols, targets, prices);
        const double new_gross = r.gross;
        auto& new_net = r.net;

        if (limits_.max_gross_exposure_usd > 0.0) {
            double gross = pm_->getGrossExposure() + reservedGrossLocked() + new_gross;
            if (gross > limits_.max_gross_exposure_usd)
                return "gross exposure " + fmt(gross) + " > limit " + fmt(limits_.max_gross_exposure_usd);
        }
        if (limits_.max_symbol_net_usd > 0.0) {
            for (size_t i = 0; i < symbols.size(); ++i) {
                const auto& sym = symbols[i];
                double net = pm_->getNetSymbolQuantity(sym) + reservedNetLocked(sym) + new_net[sym];
                double usd = std::abs(net * prices[i]);
                if (usd > limits_.max_symbol_net_usd)
                    return sym + " net exposure " + fmt(usd) + " > limit " + fmt(limits_.max_symbol_net_usd);
            }
        }
        reservations_[basket_id] = std::move(r);
        return std::nullopt;
    }

    // An entry the previous process approved whose orders were still working when this one
    // started (restart, handover): reserved again, unchecked, so the caps keep counting it.
    void reserve(const std::string& basket_id, const std::vector<std::string>& symbols,
                 const std::vector<LegTarget>& targets, const std::vector<double>& prices) {
        std::lock_guard<std::mutex> lock(mtx_);
        reservations_[basket_id] = reservationOf(symbols, targets, prices);
    }

    // Called when the basket's entry cycle resolves (filled exposure is now in the PM).
    void release(const std::string& basket_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        reservations_.erase(basket_id);
    }

    bool basketLossBreached(const std::vector<std::string>& position_keys) const {
        if (limits_.max_basket_loss_usd <= 0.0) return false;
        return pm_->getPnL(position_keys) < -limits_.max_basket_loss_usd;
    }

    bool killSwitchActive() const { return fileExists(limits_.kill_switch_file); }
    bool flattenRequested() const { return fileExists(limits_.flatten_file); }
    const RiskLimits& limits() const { return limits_; }
    // Exposure reserved by entries in flight (MOC orders not filled yet).
    double reservedGross() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return reservedGrossLocked();
    }

    // |net exposure| in USD per symbol, held plus reserved by entries in flight: what the
    // per-symbol cap is checked against (for the dashboard; approveEntry prices legs itself).
    std::map<std::string, double> symbolNetUsd() const {
        auto held = pm_->getSymbolNets();
        std::lock_guard<std::mutex> lock(mtx_);
        std::map<std::string, std::pair<double, double>> qp;  // symbol -> qty, price
        for (const auto& [sym, n] : held) qp[sym] = {n.quantity, n.price};
        for (const auto& [b, r] : reservations_) {
            for (const auto& [sym, q] : r.net) {
                auto& [qty, px] = qp[sym];
                qty += q;
                if (px <= 0.0) px = r.price.at(sym);
            }
        }
        std::map<std::string, double> out;
        for (const auto& [sym, v] : qp) {
            if (std::abs(v.first) > 1e-9) out[sym] = std::abs(v.first * v.second);
        }
        return out;
    }

private:
    struct Reservation {
        double gross;
        std::map<std::string, double> net;
        std::map<std::string, double> price;  // leg prices the entry was approved at
    };

    static Reservation reservationOf(const std::vector<std::string>& symbols, const std::vector<LegTarget>& targets,
                                     const std::vector<double>& prices) {
        Reservation r{0.0, {}, {}};
        for (size_t i = 0; i < targets.size(); ++i) {
            r.gross += targets[i].quantity * prices[i];
            r.net[symbols[i]] += targets[i].side == Side::BUY ? targets[i].quantity : -targets[i].quantity;
            r.price[symbols[i]] = prices[i];
        }
        return r;
    }

    static bool fileExists(const std::string& p) { return !p.empty() && std::filesystem::exists(p); }
    static std::string fmt(double v) { return std::to_string(static_cast<long long>(v)); }

    double reservedGrossLocked() const {
        double g = 0.0;
        for (const auto& [b, r] : reservations_) g += r.gross;
        return g;
    }
    double reservedNetLocked(const std::string& sym) const {
        double n = 0.0;
        for (const auto& [b, r] : reservations_) {
            auto it = r.net.find(sym);
            if (it != r.net.end()) n += it->second;
        }
        return n;
    }

    std::shared_ptr<PortfolioManager> pm_;
    RiskLimits limits_;
    mutable std::mutex mtx_;
    std::map<std::string, Reservation> reservations_;
};

} // namespace bot
