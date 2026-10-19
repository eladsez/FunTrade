#pragma once
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <optional>
#include <memory>

namespace bot {

class IExecutionEngine;

struct SymbolMismatch {
    std::string symbol;
    double virtual_qty;  // sum of the bot's per-basket positions
    double broker_qty;   // what the broker actually holds
};

// Tracks the bot's inventory as VIRTUAL positions, one per position key
// ("<basket_id>_<SYMBOL>"), so baskets sharing a symbol keep separate books. The broker
// only knows the net per symbol; reconcile() checks the two agree.
//
// With a state file, virtual positions and the IDs of executions already booked are
// persisted after every fill, so a restart resumes with the same books and a redelivered
// execution is not booked twice.
class PortfolioManager {
public:
    PortfolioManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine,
                     std::string stateFile = "");
    ~PortfolioManager() = default;

    void onMarketEvent(const MarketEvent& event);
    void onExecution(const ExecutionReport& report);

    double getTotalRealizedPnL() const;
    double getTotalUnrealizedPnL() const;
    double getNetPosition(const std::string& virtual_key) const;
    size_t getTotalTrades() const; // number of executions (fills, incl. partials)
    std::optional<Position> getPosition(const std::string& virtual_key) const;
    // Realized + unrealized PnL of the given virtual positions (e.g. one basket's legs).
    double getPnL(const std::vector<std::string>& virtual_keys) const;
    // Gross market value of all virtual positions, and the bot's net per symbol.
    double getGrossExposure() const;
    double getNetSymbolQuantity(const std::string& symbol) const;

    // Broker view
    double getBrokerPosition(const std::string& symbol) const;
    // Refreshes broker positions from the engine and logs the reconciliation.
    void resync();
    // Symbols where the bot's virtual books and the broker disagree.
    std::vector<SymbolMismatch> reconcile() const;
    bool isReconciled(const std::string& symbol) const;

    std::optional<double> getPortfolioValue() const;
    std::optional<double> getAvailableCash() const;

private:
    double applyFill(Position& pos, double signed_fill_qty, double fill_price);
    bool updatePositionIndex(const std::string& symbol, const std::string& virtual_key, bool was_flat, bool is_flat_now);
    void syncBrokerPositions();
    void loadState();
    void saveStateLocked() const;
    double virtualQtyLocked(const std::string& symbol) const;
    std::vector<SymbolMismatch> reconcileLocked() const;
    void logReconciliation(const char* when) const;

    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<IExecutionEngine> engine_;
    std::string state_file_;

    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, std::vector<std::string>> symbol_to_vkeys_;
    std::map<std::string, double> broker_positions_;
    mutable std::mutex mtx_;
    std::unordered_set<std::string> seen_exec_ids_;
    double total_realized_pnl_ = 0.0;
    size_t total_trades_{0};
    int64_t last_reconcile_day_ = 0;
};

} // namespace bot
