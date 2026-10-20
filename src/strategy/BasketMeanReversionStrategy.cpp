#include <strategy/BasketMeanReversionStrategy.hpp>
#include <risk/UiBroadcaster.hpp>
#include <utils/TimeUtils.hpp>
#include <utils/StateStore.hpp>
#include <core/Logger.hpp>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <thread>
#include <utils/magic_enum.hpp>

namespace bot {

namespace {

uint64_t timestampOf(const MarketEvent& e) {
    return std::visit([](auto&& t) { return t.timestamp_ms; }, e);
}

// First day whose daily bar may still be forming: today in New York until the EOD fetch
// (18:00 ET), tomorrow after it. Keyed like tradingDay(), which is the bar's calendar date.
int64_t firstUnsettledDay() {
    using namespace std::chrono;
    auto ny = locate_zone("America/New_York")->to_local(system_clock::now());
    auto day = floor<days>(ny);
    if (ny - day >= hours{18}) day += days{1};
    return day.time_since_epoch().count();
}

template <typename E>
std::string name(E e) { return std::string(magic_enum::enum_name(e)); }

} // namespace

BasketMeanReversionStrategy::BasketMeanReversionStrategy(
    std::shared_ptr<IEventBus> eventBus,
    std::shared_ptr<OrderManager> orderMngr,
    std::shared_ptr<PortfolioManager> portfolioMngr,
    const std::string& configFilePath,
    double capitalAllocation,
    StrategyOptions options)
    : event_bus_(std::move(eventBus)),
      order_mngr_(std::move(orderMngr)),
      portfolio_mngr_(std::move(portfolioMngr)),
      options_(std::move(options)),
      config_path_(configFilePath),
      capital_allocation_(capitalAllocation) {}

// =========================================================================
// Lifecycle
// =========================================================================

void BasketMeanReversionStrategy::start() {
    loadConfig();
    loadState();
    warmUp();

    const auto& symbols = basket_state_->getSymbols();
    for (const auto& sym : symbols) {
        uint64_t subid = event_bus_->subscribeToMarketEvents(
            sym, [this](const MarketEvent& e) { this->onMarketEvent(e); });
        market_subid_.emplace(sym, subid);
    }
    report_subid_ = event_bus_->subscribeToExecution(
        [this](const ExecutionReport& report) { this->onExecutionReport(report); });

    UiBroadcaster::getInstance().broadcastNewBasket(basket_id_, symbols, basket_state_->getWeights(),
        -entry_z_score_, -exit_z_score_, exit_z_score_, entry_z_score_);

    // Reconcile on the dispatcher, ordered after anything already queued.
    event_bus_->post([this] { reconcileOnStart(); });
}

void BasketMeanReversionStrategy::stop() {
    // 1. Cancel whatever is still working (on the dispatcher). A partially filled entry then
    //    resolves into a recovery scheduled for the next session; positions are NOT
    //    flattened: they are persisted and resumed on restart.
    event_bus_->post([this] {
        if (!current_cycle_) return;
        for (const auto& cid : current_cycle_->workingClientOrderIds()) order_mngr_->cancelOrder(cid);
    });

    // 2. Wait (on this thread) for the cancels to resolve the cycle.
    auto deadline = std::chrono::steady_clock::now() + kShutdownTimeout;
    while (cycle_active_ && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // 3. Persist and detach (on the dispatcher).
    event_bus_->post([this] {
        if (current_cycle_) {
            LOG_ERROR("[{}] Shutdown: cycle did not resolve; it will be reattached on restart.", basket_id_);
        } else if (hasInventory()) {
            LOG_INFO("[{}] Shutdown with open position ({}); state saved for restart.", basket_id_, name(master_state_));
        } else {
            LOG_INFO("[{}] Flat at shutdown.", basket_id_);
        }
        saveState();
        for (const auto& [sym, id] : market_subid_) event_bus_->unsubscribeFromMarketEvents(sym, id);
        event_bus_->unsubscribeFromExecution(report_subid_);
    });
    event_bus_->flush();
}

void BasketMeanReversionStrategy::loadConfig() {
    std::ifstream file(config_path_);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + config_path_);
    }

    nlohmann::json config;
    file >> config;

    // Parse nested structure from the Python export
    basket_id_ = config["basket_id"];
    int window_size = config["trading_params"]["window_size"];
    entry_z_score_ = config["trading_params"]["entry_z"];
    exit_z_score_ = config["trading_params"]["exit_z"];

    std::vector<std::string> symbols;
    std::vector<double> weights;
    for (const auto& ticker : config["tickers"]) {
        std::string sym = ticker.get<std::string>();
        symbols.push_back(sym);
        weights.push_back(config["weights"][sym].get<double>());
        position_keys_.push_back(basket_id_ + "_" + sym);
    }

    basket_state_ = std::make_unique<BasketState>(window_size, symbols, weights, basket_id_);
}

// =========================================================================
// History
// =========================================================================

// Loads daily closes from the feed. With `up_to_day` (the close being processed) it loads
// bars up to that day. Without it (warm-up) it skips bars that may still be forming.
void BasketMeanReversionStrategy::loadHistory(std::optional<int64_t> up_to_day) {
    auto now = std::chrono::system_clock::now();
    // ~1.45 calendar days per trading day; 2x plus slack covers holidays.
    int calendar_days_back = basket_state_->getWindowSize() * 2 + 10;
    auto start = now - std::chrono::hours(24 * calendar_days_back);
    std::string end_time = utils::formatTimePoint(now);
    std::string start_time = utils::formatTimePoint(start);
    int64_t unsettled = firstUnsettledDay();

    const auto& symbols = basket_state_->getSymbols();
    std::vector<std::vector<HistoricalBar>> all(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        all[i] = event_bus_->fetchHistoricalBars(symbols[i], start_time, end_time, "1Day");
    }

    for (size_t i = 0; i < symbols.size(); ++i) {
        for (const auto& bar : all[i]) {
            int64_t day = tradingDay(bar.timestamp_ms);
            if (up_to_day ? day > *up_to_day : day >= unsettled) continue;
            basket_state_->setDailyClose(symbols[i], day, bar.close);
        }
    }
}

void BasketMeanReversionStrategy::warmUp() {
    loadHistory(std::nullopt);

    // Seed live prices with the latest closes so sizing works before the first tick.
    if (auto last = basket_state_->latestCompleteDay()) {
        last_close_day_ = *last;
        session_day_ = *last;
        const auto& symbols = basket_state_->getSymbols();
        auto closes = basket_state_->closesOn(*last);
        for (size_t i = 0; i < symbols.size(); ++i) {
            basket_state_->updatePrice(symbols[i], (*closes)[i], 0);
        }
    }

    size_t days = basket_state_->completeDayCount();
    if (days < static_cast<size_t>(basket_state_->getWindowSize())) {
        LOG_WARN("[{}] Warm-up: only {} of {} days of history. No signal until the window fills.",
                 basket_id_, days, basket_state_->getWindowSize());
    } else if (last_close_day_ > 0) {
        last_daily_z_ = basket_state_->dailyZScore(last_close_day_);
        LOG_INFO("[{}] Warmed up on {} days. Last close z = {:.4f}", basket_id_, days, last_daily_z_.value_or(0.0));
    }
}

// =========================================================================
// Events
// =========================================================================

bool BasketMeanReversionStrategy::isValidMarketEvent(const MarketEvent& event) const {
    if (const auto* trade = std::get_if<TradeTick>(&event)) {
        return trade->price > 0.0;
    } else if (const auto* quote = std::get_if<QuoteTick>(&event)) {
        // No zero prices or crossed markets, and no absurd spreads (> 5%).
        if (quote->bid_price <= 0.0 || quote->ask_price <= 0.0 || quote->bid_price >= quote->ask_price) {
            return false;
        }
        return (quote->ask_price - quote->bid_price) / quote->ask_price < 0.05;
    } else if (const auto* midTick = std::get_if<MidPointTick>(&event)) {
        return midTick->mid_price > 0;
    } else if (const auto* bar = std::get_if<BarUpdate>(&event)) {
        return bar->close > 0.0;
    }
    return false;
}

void BasketMeanReversionStrategy::onMarketEvent(const MarketEvent& event) {
    if (!isValidMarketEvent(event)) return;

    uint64_t ts = timestampOf(event);
    int64_t day = tradingDay(ts);

    // The first event of a new trading day starts that day's session.
    if (ts > 0 && day > session_day_) {
        session_day_ = day;
        onNewSession();
    }

    if (const auto* bar = std::get_if<BarUpdate>(&event)) {
        basket_state_->setDailyClose(bar->symbol, day, bar->close);
        basket_state_->updatePrice(bar->symbol, bar->close, ts);
        if (day > last_close_day_ && basket_state_->isDayComplete(day)) {
            last_close_day_ = day;
            onDailyClose(day);
        }
        return;
    }

    double price = 0.0;
    std::string symbol;
    if (const auto* t = std::get_if<TradeTick>(&event)) { price = t->price; symbol = t->symbol; }
    else if (const auto* q = std::get_if<QuoteTick>(&event)) { price = 0.5 * (q->bid_price + q->ask_price); symbol = q->symbol; }
    else if (const auto* m = std::get_if<MidPointTick>(&event)) { price = m->mid_price; symbol = m->symbol; }
    basket_state_->updatePrice(symbol, price, ts);
    logIndicativeZThrottled(ts);
}

void BasketMeanReversionStrategy::onExecutionReport(const ExecutionReport& report) {
    if (!current_cycle_) return;
    if (current_cycle_->processExecutionReport(report)) {
        evaluateCompletedCycle();
    }
}

// =========================================================================
// Daily cycle
// =========================================================================

void BasketMeanReversionStrategy::onNewSession() {
    if (pending_action_ == PendingAction::NONE) return;
    if (current_cycle_) {
        LOG_WARN("[{}] Session start: previous cycle still unresolved; {} deferred.", basket_id_, name(pending_action_));
        return;
    }

    PendingAction action = pending_action_;
    pending_action_ = PendingAction::NONE;
    switch (action) {
        case PendingAction::ENTER_LONG:  enter(PositionState::LONG); break;
        case PendingAction::ENTER_SHORT: enter(PositionState::SHORT); break;
        case PendingAction::EXIT:        exitBasket(MasterState::EXIT_PENDING); break;
        case PendingAction::RECOVER:     exitBasket(MasterState::RECOVERY_PENDING); break;
        case PendingAction::NONE: break;
    }
    saveState();
    broadcastState();
}

void BasketMeanReversionStrategy::onDailyClose(int64_t day) {
    // Watchdog: MOC orders from an earlier session must have resolved by now. Cancel what's
    // left; the cancel reports resolve the cycle through the normal path.
    if (current_cycle_ && cycle_day_ < day) {
        LOG_ERROR("[{}] Watchdog: cycle from day {} still unresolved at close of day {}. Canceling working legs.",
                  basket_id_, cycle_day_, day);
        for (const auto& cid : current_cycle_->workingClientOrderIds()) order_mngr_->cancelOrder(cid);
    }

    if (options_.rebuild_window_daily) {
        // Re-read (dividend-adjusted) history so the window matches the research data.
        BasketState before = *basket_state_;
        basket_state_->clearDailyHistory();
        loadHistory(day);
        if (basket_state_->completeDayCount() < static_cast<size_t>(basket_state_->getWindowSize()) ||
            !basket_state_->isDayComplete(day)) {
            LOG_WARN("[{}] History rebuild incomplete; keeping incremental closes.", basket_id_);
            *basket_state_ = before;
        }
    }

    auto& risk = options_.risk;
    if (risk && master_state_ == MasterState::IN_POSITION && !halt_after_flat_ &&
        risk->basketLossBreached(position_keys_)) {
        halt_after_flat_ = true;
        LOG_ERROR("[{}] Basket loss limit breached (PnL {:.2f}). Exiting, then halting.",
                  basket_id_, portfolio_mngr_->getPnL(position_keys_));
    }

    decide(day);
    saveState();
}

void BasketMeanReversionStrategy::decide(int64_t day) {
    if (day <= decision_day_) return;  // already decided on this close (e.g. before a restart)
    // A cycle in flight (fills can land after the close bar) means the position isn't known
    // yet: don't consume this close. evaluateCompletedCycle() decides once it resolves.
    if (current_cycle_) return;
    decision_day_ = day;

    last_daily_z_ = basket_state_->dailyZScore(day);
    if (last_daily_z_) {
        UiBroadcaster::getInstance().broadcastZTick(basket_id_, *last_daily_z_);
    }

    auto& risk = options_.risk;
    bool flatten = risk && risk->flattenRequested();

    switch (master_state_) {
        case MasterState::FLAT: {
            if (!last_daily_z_ || flatten || halt_after_flat_) break;
            double z = *last_daily_z_;
            if (z > entry_z_score_) pending_action_ = PendingAction::ENTER_SHORT;
            else if (z < -entry_z_score_) pending_action_ = PendingAction::ENTER_LONG;
            break;
        }
        case MasterState::IN_POSITION: {
            if (flatten || halt_after_flat_) { pending_action_ = PendingAction::EXIT; break; }
            if (!last_daily_z_) break;
            double z = *last_daily_z_;
            if (current_position_ == PositionState::SHORT && z <= exit_z_score_) pending_action_ = PendingAction::EXIT;
            if (current_position_ == PositionState::LONG && z >= -exit_z_score_) pending_action_ = PendingAction::EXIT;
            break;
        }
        default:
            break;  // pending cycles resolve themselves; RECOVERY keeps its scheduled RECOVER
    }

    if (pending_action_ != PendingAction::NONE) {
        LOG_INFO("[{}] Close z = {:.4f} -> {} at next session.", basket_id_, last_daily_z_.value_or(0.0), name(pending_action_));
    }
}

// =========================================================================
// Execution
// =========================================================================

void BasketMeanReversionStrategy::enter(PositionState side) {
    size_t n = basket_state_->getNumComponents();
    std::vector<double> prices(n);
    for (size_t i = 0; i < n; ++i) prices[i] = basket_state_->getPrice(i);

    if (!basket_state_->hasAllPrices()) {
        LOG_WARN("[{}] Entry skipped: no price for every leg yet.", basket_id_);
        return;
    }

    auto targets = BasketSizer::calculateEntry(side, capital_allocation_, basket_state_->getWeights(), prices);
    if (!targets) {
        LOG_INFO("[{}] BasketSizer rejected entry.", basket_id_);
        return;
    }

    if (options_.risk) {
        if (auto reason = options_.risk->approveEntry(basket_id_, basket_state_->getSymbols(), *targets, prices)) {
            LOG_WARN("[{}] Risk rejected entry: {}", basket_id_, *reason);
            return;
        }
    }

    master_state_ = MasterState::ENTRY_PENDING;
    current_position_ = side;
    dispatchCycle(*targets, "ENTRY");
}

void BasketMeanReversionStrategy::exitBasket(MasterState as) {
    master_state_ = as;  // set first: the cycle may resolve synchronously
    dispatchCycle(BasketSizer::calculateExit(position_keys_, *portfolio_mngr_),
                  as == MasterState::RECOVERY_PENDING ? "RECOVERY" : "EXIT");
}

// Starts a new execution cycle: one MARKET_ON_CLOSE order per non-zero leg, each with a
// fresh client order id so late reports from earlier cycles can never be attributed here.
void BasketMeanReversionStrategy::dispatchCycle(const std::vector<LegTarget>& targets, const char* label) {
    const auto& symbols = basket_state_->getSymbols();
    current_cycle_ = std::make_unique<ExecutionCycle>(basket_id_);
    cycle_active_ = true;
    cycle_day_ = session_day_;
    cycle_legs_.clear();

    std::vector<OrderRequest> orders;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].quantity <= 0.0) continue;
        OrderRequest req;
        req.client_order_id = OrderIdUtils::newClientOrderId(position_keys_[i]);
        req.symbol = symbols[i];
        req.side = targets[i].side;
        req.type = OrderType::MARKET_ON_CLOSE;
        req.quantity = targets[i].quantity;
        req.tif = TimeInForce::DAY;
        current_cycle_->addLeg(req.client_order_id, req.symbol, req.quantity, req.side);
        cycle_legs_.push_back({req.client_order_id, req.symbol, position_keys_[i], req.side, req.quantity,
                               portfolio_mngr_->getNetPosition(position_keys_[i])});
        orders.push_back(std::move(req));
    }
    saveState();  // the cycle is on disk before any order leaves

    // Every leg is registered before the first order goes out, so a fast fill can't
    // complete the cycle early.
    for (const auto& req : orders) {
        LOG_INFO("[{}] {} Routing: {} {} {:.4f} shares (MOC).", basket_id_, label,
                 (req.side == Side::BUY ? "BUY" : "SELL"), req.symbol, req.quantity);

        if (order_mngr_->submitOrder(req).empty() && current_cycle_) {
            // Never reached the exchange: no report will ever come, so resolve the leg now.
            ExecutionReport rejected;
            rejected.client_order_id = req.client_order_id;
            rejected.symbol = req.symbol;
            rejected.side = req.side;
            rejected.status = OrderStatus::REJECTED;
            if (current_cycle_->processExecutionReport(rejected)) {
                evaluateCompletedCycle();
            }
        }
    }

    // Nothing to trade (e.g. exit with no inventory) resolves immediately.
    if (current_cycle_ && current_cycle_->isComplete()) {
        evaluateCompletedCycle();
    }
}

void BasketMeanReversionStrategy::resetCycle() {
    current_cycle_.reset();
    cycle_legs_.clear();
    cycle_active_ = false;
    if (options_.risk) options_.risk->release(basket_id_);
}

void BasketMeanReversionStrategy::halt(const std::string& reason) {
    LOG_ERROR("[{}] SYSTEM HALTED: {}", basket_id_, reason);
    master_state_ = MasterState::SYSTEM_HALTED;
    pending_action_ = PendingAction::NONE;
    halt_reason_ = reason;
    saveState();
    broadcastState();
}

void BasketMeanReversionStrategy::evaluateCompletedCycle() {
    if (!current_cycle_) return;

    // Scenario 0: the cycle had nothing to trade (all targets 0, e.g. exit with no inventory).
    if (!current_cycle_->hasLegs()) {
        if (master_state_ == MasterState::EXIT_PENDING || master_state_ == MasterState::RECOVERY_PENDING) {
            LOG_WARN("[{}] No inventory left to trade. State -> FLAT.", basket_id_);
        }
        master_state_ = MasterState::FLAT;
        current_position_ = PositionState::FLAT;
    } else {
        bool has_fills = current_cycle_->hasAnyFills();
        double missed_shares = current_cycle_->getMissedShares();

        if (has_fills && missed_shares <= 1e-6) {
            // Scenario A: perfect execution
            if (master_state_ == MasterState::ENTRY_PENDING) {
                master_state_ = MasterState::IN_POSITION;
                LOG_INFO("[{}] Entry cycle perfect. State -> IN_POSITION.", basket_id_);
            } else {
                master_state_ = MasterState::FLAT;
                current_position_ = PositionState::FLAT;
                LOG_INFO("[{}] Exit/Recovery cycle perfect. State -> FLAT.", basket_id_);
            }
        } else if (!has_fills) {
            // Scenario B: complete whiff
            if (master_state_ == MasterState::ENTRY_PENDING) {
                LOG_WARN("[{}] Entry completely whiffed. Zero fills. Reverting State -> FLAT.", basket_id_);
                master_state_ = MasterState::FLAT;
                current_position_ = PositionState::FLAT;
            } else if (master_state_ == MasterState::EXIT_PENDING) {
                LOG_WARN("[{}] Exit completely whiffed. Zero fills. Reverting State -> IN_POSITION.", basket_id_);
                master_state_ = MasterState::IN_POSITION;
                decision_day_ = 0;  // re-decide at the next close
            } else if (master_state_ == MasterState::RECOVERY_PENDING) {
                resetCycle();
                halt("recovery sweep got zero fills; exchange rejected the orders");
                return;
            }
        } else {
            // Scenario C: partial fill -> basket unhedged
            if (master_state_ == MasterState::RECOVERY_PENDING) {
                resetCycle();
                halt("recovery sweep only partially filled; residual inventory remains");
                return;
            }
            LOG_ERROR("[{}] Partial fill (missed {:.4f} shares). Basket unhedged: recovery at next session.",
                      basket_id_, missed_shares);
            master_state_ = MasterState::RECOVERY_PENDING;
            pending_action_ = PendingAction::RECOVER;
        }
    }

    resetCycle();

    if (halt_after_flat_ && master_state_ == MasterState::FLAT) {
        halt("basket loss limit breached; position exited");
        return;
    }
    // A close that arrived while this cycle was in flight hasn't been decided on yet.
    if (pending_action_ == PendingAction::NONE && last_close_day_ > decision_day_) {
        decide(last_close_day_);
    }
    saveState();
    broadcastState();
}

bool BasketMeanReversionStrategy::hasInventory() const {
    for (const auto& key : position_keys_) {
        if (std::abs(portfolio_mngr_->getNetPosition(key)) > 1e-9) return true;
    }
    return false;
}

// =========================================================================
// Persistence & restart
// =========================================================================

std::string BasketMeanReversionStrategy::statePath() const {
    if (options_.state_dir.empty()) return "";
    return (std::filesystem::path(options_.state_dir) / (basket_id_ + ".json")).string();
}

void BasketMeanReversionStrategy::saveState() const {
    std::string path = statePath();
    if (path.empty()) return;
    nlohmann::json j;
    j["basket_id"] = basket_id_;
    j["master_state"] = name(master_state_);
    j["position"] = name(current_position_);
    j["pending_action"] = name(pending_action_);
    j["decision_day"] = decision_day_;
    j["halt_after_flat"] = halt_after_flat_;
    j["halt_reason"] = halt_reason_;
    j["cycle_day"] = cycle_day_;
    j["cycle_legs"] = nlohmann::json::array();
    for (const auto& l : cycle_legs_) {
        j["cycle_legs"].push_back({{"client_order_id", l.client_order_id}, {"symbol", l.symbol},
                                   {"position_key", l.position_key}, {"side", name(l.side)},
                                   {"target", l.target}, {"baseline", l.baseline}});
    }
    state::writeJsonAtomic(path, j);
}

void BasketMeanReversionStrategy::loadState() {
    std::string path = statePath();
    if (path.empty()) return;
    auto j = state::readJson(path);  // corrupt state throws: never trade on unknown state
    if (!j) return;

    master_state_ = magic_enum::enum_cast<MasterState>(j->value("master_state", "FLAT")).value_or(MasterState::SYSTEM_HALTED);
    current_position_ = magic_enum::enum_cast<PositionState>(j->value("position", "FLAT")).value_or(PositionState::FLAT);
    pending_action_ = magic_enum::enum_cast<PendingAction>(j->value("pending_action", "NONE")).value_or(PendingAction::NONE);
    decision_day_ = j->value("decision_day", int64_t{0});
    halt_after_flat_ = j->value("halt_after_flat", false);
    halt_reason_ = j->value("halt_reason", "");
    cycle_day_ = j->value("cycle_day", int64_t{0});
    for (const auto& l : j->value("cycle_legs", nlohmann::json::array())) {
        cycle_legs_.push_back({l.at("client_order_id"), l.at("symbol"), l.at("position_key"),
                               magic_enum::enum_cast<Side>(l.at("side").get<std::string>()).value_or(Side::UNKNOWN),
                               l.at("target"), l.at("baseline")});
    }
    LOG_INFO("[{}] Restored state {} / {} (pending {}, {} in-flight legs).", basket_id_,
             name(master_state_), name(current_position_), name(pending_action_), cycle_legs_.size());
}

// A cycle that was in flight when the bot stopped. Each leg's fill so far is what the
// virtual books moved since the cycle started; legs whose order is still working at the
// broker keep receiving reports.
void BasketMeanReversionStrategy::restoreCycle() {
    current_cycle_ = std::make_unique<ExecutionCycle>(basket_id_);
    cycle_active_ = true;
    for (const auto& l : cycle_legs_) {
        double filled = std::abs(portfolio_mngr_->getNetPosition(l.position_key) - l.baseline);
        auto order = order_mngr_->getOrder(l.client_order_id);
        bool working = order && !isTerminal(order->status);
        OrderStatus st = working ? OrderStatus::NEW
                       : (filled + 1e-9 >= l.target ? OrderStatus::FILLED : OrderStatus::CANCELED);
        current_cycle_->restoreLeg(l.client_order_id, l.symbol, l.target, l.side, filled, st);
        LOG_INFO("[{}] Reattached leg {}: {:.4f}/{:.4f} filled, {}", basket_id_, l.client_order_id,
                 filled, l.target, working ? "still working" : "done");
    }
    if (current_cycle_->isComplete()) evaluateCompletedCycle();
}

void BasketMeanReversionStrategy::reconcileOnStart() {
    if (master_state_ == MasterState::SYSTEM_HALTED) {
        LOG_ERROR("[{}] Starting HALTED ({}). Fix the cause and delete {} to resume.", basket_id_, halt_reason_, statePath());
        return;
    }

    for (const auto& sym : basket_state_->getSymbols()) {
        if (!portfolio_mngr_->isReconciled(sym)) {
            halt("broker position in " + sym + " (" + std::to_string(portfolio_mngr_->getBrokerPosition(sym)) +
                 ") differs from the bot's books (" + std::to_string(portfolio_mngr_->getNetSymbolQuantity(sym)) + ")");
            return;
        }
    }

    bool pending_cycle = master_state_ == MasterState::ENTRY_PENDING || master_state_ == MasterState::EXIT_PENDING ||
                         (master_state_ == MasterState::RECOVERY_PENDING && !cycle_legs_.empty());
    if (pending_cycle) {
        if (cycle_legs_.empty()) {
            // Crashed between deciding and recording the cycle: resolve from inventory.
            master_state_ = hasInventory() ? MasterState::RECOVERY_PENDING : MasterState::FLAT;
            if (master_state_ == MasterState::RECOVERY_PENDING) pending_action_ = PendingAction::RECOVER;
        } else {
            restoreCycle();
        }
    } else if (master_state_ == MasterState::IN_POSITION && !hasInventory()) {
        LOG_WARN("[{}] State says IN_POSITION but there is no inventory. State -> FLAT.", basket_id_);
        master_state_ = MasterState::FLAT;
        current_position_ = PositionState::FLAT;
    } else if (master_state_ == MasterState::FLAT && hasInventory()) {
        halt("inventory exists for a basket whose state is FLAT");
        return;
    }

    // Decide on the latest close if that hasn't happened yet (e.g. first run, or down at EOD).
    if (last_close_day_ > 0 && pending_action_ == PendingAction::NONE) decide(last_close_day_);
    saveState();
    broadcastState();
}

// =========================================================================
// UI
// =========================================================================

void BasketMeanReversionStrategy::broadcastState() const {
    UiBroadcaster::getInstance().broadcastState(basket_id_, name(master_state_), name(current_position_));
}

void BasketMeanReversionStrategy::logIndicativeZThrottled(uint64_t timestamp) {
    if (timestamp - last_z_log_time_ms_ < 500) return;
    last_z_log_time_ms_ = timestamp;
    if (auto z = basket_state_->indicativeZScore()) {
        LOG_DEBUG("[{}] Indicative intraday z: {:.4f} (decisions use the close)", basket_id_, *z);
        UiBroadcaster::getInstance().broadcastZTick(basket_id_, *z);
    }
}

} // namespace bot
