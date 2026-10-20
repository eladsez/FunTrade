#pragma once

#include <core/IEventBus.hpp>
#include <data/IDataFeed.hpp>
#include <execution/OrderManager.hpp>
#include <strategy/IStrategy.hpp>
#include <strategy/BasketState.hpp>
#include <risk/PortfolioManager.hpp>
#include <risk/RiskManager.hpp>
#include <strategy/ExecutionCycle.hpp>
#include <strategy/BasketSizer.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bot {

enum class MasterState {
    FLAT,               // Clean inventory, hunting for entry signals
    ENTRY_PENDING,      // Orders dispatched, waiting for exchange resolution
    IN_POSITION,        // Fully confirmed, balanced basket
    EXIT_PENDING,       // Closing orders dispatched, waiting for resolution
    RECOVERY_PENDING,   // Unbalanced inventory: flattening it (sweep scheduled or in flight)
    SYSTEM_HALTED       // Fatal error state: needs a human (see halt reason in the state file)
};

// What the strategy will do at the start of the next session, decided at the previous close.
enum class PendingAction { NONE, ENTER_LONG, ENTER_SHORT, EXIT, RECOVER };

struct StrategyOptions {
    // Directory for per-basket state files; empty disables persistence (tests, backtests).
    std::string state_dir;
    // Rebuild the z-score window from the feed's (dividend-adjusted) daily history at every
    // close, instead of appending raw closes. Live IBKR: true. CSV backtest: false (its
    // history file only covers the warm-up period).
    bool rebuild_window_daily = false;
    std::shared_ptr<RiskManager> risk;  // optional
    // Directory for <basket>.json with the bot's own z for every day of a longer warm-up
    // history (display only: the dashboard's z chart). Empty disables it (tests, backtests),
    // and then warm-up loads exactly what it always did.
    std::string history_dir;
};

// Daily basket mean reversion, trading exactly the rule validated by the research
// simulator (scripts/compute.py):
//   * at the close of day d (every leg has a daily bar), compute z(d) and decide;
//   * at the first event of day d+1, act with MARKET_ON_CLOSE orders, which fill at the
//     close of d+1 -- the fill price the simulator assumes.
// Entry: z > entry_z -> short the spread, z < -entry_z -> long. Exit: short when z <= exit_z,
// long when z >= -exit_z. All state changes are persisted; on start the strategy reconciles
// with the broker and resumes (or halts) instead of starting blind.
//
// Threading: all strategy state is touched on the event dispatcher thread only. stop() is
// safe to call from any other thread.
class BasketMeanReversionStrategy : public IStrategy {
public:
    BasketMeanReversionStrategy(
        std::shared_ptr<IEventBus> eventBus,
        std::shared_ptr<OrderManager> orderMngr,
        std::shared_ptr<PortfolioManager> portfolioMngr,
        const std::string& configFilePath,
        double capitalAllocation,
        StrategyOptions options = {});

    void start() override;
    void stop() override;

    // Introspection (dispatcher thread, or tests on a synchronous bus)
    MasterState masterState() const { return master_state_; }
    PositionState position() const { return current_position_; }
    PendingAction pendingAction() const { return pending_action_; }
    std::optional<double> lastDailyZ() const { return last_daily_z_; }
    const std::string& basketId() const { return basket_id_; }
    const std::string& haltReason() const { return halt_reason_; }

private:
    void loadConfig();
    void warmUp();
    void loadHistory(std::optional<int64_t> up_to_day);
    std::vector<std::vector<HistoricalBar>> fetchDailyBars(int calendar_days_back) const;
    void applyBars(BasketState& into, const std::vector<std::vector<HistoricalBar>>& bars,
                   std::optional<int64_t> up_to_day, int64_t from_day) const;
    int tradingCalendarDays() const;
    void writeZHistory(const BasketState& full) const;
    void onMarketEvent(const MarketEvent& event) override;
    void onExecutionReport(const ExecutionReport& report);
    bool isValidMarketEvent(const MarketEvent& event) const;

    // Daily cycle
    void onNewSession();
    void onDailyClose(int64_t day);
    void decide(int64_t day);

    // Execution
    void enter(PositionState side);
    void exitBasket(MasterState as);
    void dispatchCycle(const std::vector<LegTarget>& targets, const char* label);
    void evaluateCompletedCycle();
    void resetCycle();
    void halt(const std::string& reason);

    // Persistence & restart
    void reconcileOnStart();
    void restoreCycle();
    void saveState() const;
    void loadState();
    std::string statePath() const;

    bool hasInventory() const;
    void logIndicativeZThrottled(uint64_t timestamp);
    void broadcastState() const;

    std::shared_ptr<IEventBus> event_bus_;
    std::shared_ptr<OrderManager> order_mngr_;
    std::shared_ptr<PortfolioManager> portfolio_mngr_;
    StrategyOptions options_;
    std::unique_ptr<ExecutionCycle> current_cycle_;
    std::atomic<bool> cycle_active_{false};  // readable from stop()'s thread

    std::string config_path_;
    std::unique_ptr<BasketState> basket_state_;

    double entry_z_score_ = 2.0;
    double exit_z_score_ = 0.5;

    MasterState master_state_ = MasterState::FLAT;
    PositionState current_position_ = PositionState::FLAT;
    PendingAction pending_action_ = PendingAction::NONE;
    bool halt_after_flat_ = false;   // basket loss limit hit: exit, then stay halted
    std::string halt_reason_;

    int64_t session_day_ = 0;    // trading day of the latest event seen
    int64_t last_close_day_ = 0; // latest day whose close was processed
    int64_t decision_day_ = 0;   // close the pending/last decision was based on
    std::optional<double> last_daily_z_;

    // In-flight cycle bookkeeping, persisted so a restart can reattach.
    struct LegRecord {
        std::string client_order_id;
        std::string symbol;
        std::string position_key;
        Side side = Side::UNKNOWN;
        double target = 0.0;
        double baseline = 0.0;  // virtual position before the cycle started
    };
    std::vector<LegRecord> cycle_legs_;
    int64_t cycle_day_ = 0;

    // Virtual position key per leg ("<basket_id>_<SYMBOL>"). Orders get unique client IDs
    // derived from these (OrderIdUtils::newClientOrderId); positions are tracked under them.
    std::vector<std::string> position_keys_;

    uint64_t report_subid_ = 0;
    std::unordered_map<std::string, uint64_t> market_subid_;

    std::string basket_id_;
    double capital_allocation_;
    uint64_t last_z_log_time_ms_ = 0;

    static constexpr std::chrono::milliseconds kShutdownTimeout{10000};
};

} // namespace bot
