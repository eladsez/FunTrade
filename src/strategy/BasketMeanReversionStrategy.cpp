#include <strategy/BasketMeanReversionStrategy.hpp>
#include <risk/UiBroadcaster.hpp>
#include <utils/TimeUtils.hpp>
#include <core/Logger.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utils/magic_enum.hpp>

namespace bot {

BasketMeanReversionStrategy::BasketMeanReversionStrategy(
    std::shared_ptr<IEventBus> eventBus,
    std::shared_ptr<OrderManager> orderMngr,
    std::shared_ptr<PortfolioManager> portfolioMngr,
    const std::string& configFilePath,
    double capitalAllocation)
    : event_bus_(std::move(eventBus)),
      order_mngr_(std::move(orderMngr)),
      portfolio_mngr_(std::move(portfolioMngr)),
      config_path_(configFilePath),
      capital_allocation_(capitalAllocation) {}

void BasketMeanReversionStrategy::start() {
    loadConfig();
    warmUp();

    // 1. Subscribe to real-time events for our basket components
    const auto& symbols = basket_state_->getSymbols();
    for (const auto& sym : symbols) {
        uint64_t subid = event_bus_->subscribeToMarketEvents(
            sym, 
            [this](const MarketEvent& e) { this->onMarketEvent(e); }
        );
        market_subid_.emplace(sym, subid);
    }

    // 2. Listen for execution reports to confirm our orders
    report_subid_ = event_bus_->subscribeToExecution(
        [this](const ExecutionReport& report) { this->onExecutionReport(report); }
    );

    // 3. BroadCast to UI
    UiBroadcaster::getInstance().broadcastNewBasket(basket_id_, symbols, basket_state_->getWeights(),
        -entry_z_score_, -exit_z_score_, exit_z_score_, entry_z_score_);
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

    // The python script exports arrays and dicts for tickers and weights
    for (const auto& ticker : config["tickers"]) {
        std::string sym = ticker.get<std::string>();
        symbols.push_back(sym);
        weights.push_back(config["weights"][sym].get<double>());
        
        position_keys_.push_back(config["basket_id"].get<std::string>() + "_" + sym);
    }

    basket_state_ = std::make_unique<BasketState>(window_size, symbols, weights, basket_id_);
}


std::map<uint64_t, std::vector<double>> BasketMeanReversionStrategy::alignHistoricalBars(
    const std::vector<std::vector<HistoricalBar>>& all_bars, 
    size_t num_components) const {
    
    std::map<uint64_t, std::vector<double>> aligned_closes;

    for (size_t i = 0; i < num_components; ++i) {
        for (const auto& bar : all_bars[i]) {
            // operator[] auto-creates an empty vector if the timestamp is new
            if (aligned_closes[bar.timestamp_ms].empty()) {
                aligned_closes[bar.timestamp_ms].resize(num_components, 0.0);
            }
            aligned_closes[bar.timestamp_ms][i] = bar.close;
        }
    }
    return aligned_closes;
}

void BasketMeanReversionStrategy::warmUp() {
    auto now = std::chrono::system_clock::now();
    int calendar_days_back = basket_state_->getWindowSize() * 2; 
    auto start = now - std::chrono::hours(24 * calendar_days_back);

    std::string end_time = utils::formatTimePoint(now);
    std::string start_time = utils::formatTimePoint(start);

    size_t num_components = basket_state_->getNumComponents();
    const auto& symbols = basket_state_->getSymbols();
    const auto& weights = basket_state_->getWeights();

    // Step 1: Fetch all raw historical bars
    std::vector<std::vector<HistoricalBar>> all_bars(num_components);
    for (size_t i = 0; i < num_components; ++i) {
        all_bars[i] = event_bus_->fetchHistoricalBars(symbols[i], start_time, end_time, "1Day");
    }

    // Step 2: Align bars chronologically
    auto aligned_closes = alignHistoricalBars(all_bars, num_components);

    // Step 3: Calculate spreads and prime the state machine
    for (const auto& [timestamp, prices] : aligned_closes) {
        
        // Use standard algorithm to cleanly check if all components have valid prices
        bool is_valid_day = std::all_of(prices.begin(), prices.end(), [](double p) { return p > 0.0; });
        if (!is_valid_day) continue; 

        double historical_spread = 0.0;
        for (size_t i = 0; i < num_components; ++i) {
            historical_spread += std::log(prices[i]) * weights[i];
            basket_state_->updatePrice(symbols[i], prices[i], timestamp);
        }

        basket_state_->feedHistoricalSpread(historical_spread);
    }

    basket_state_->setWarmedUp(true);

    std::optional<double> initial_z_score = basket_state_->getZScore();
    if (initial_z_score.has_value()) {
         LOG_INFO("[BasketMeanReversionStrategy] {} Warmed Up. Initial Z-Score: {}", basket_id_, initial_z_score.value());
    } else {
        LOG_WARN("[BasketMeanReversionStrategy] {} REST history empty. Strategy will warm up organically from the live stream.", basket_id_);
    }
}

bool BasketMeanReversionStrategy::isValidMarketEvent(const MarketEvent& event) const {
    if (const auto* trade = std::get_if<TradeTick>(&event)) {
        // Trade Defense: Must have a real positive price
        return trade->price > 0.0;
    } else if (const auto* quote = std::get_if<QuoteTick>(&event)) {
        // Quote Defense 1: No zero prices or crossed markets
        if (quote->bid_price <= 0.0 || quote->ask_price <= 0.0 || quote->bid_price >= quote->ask_price) {
            return false;
        }
        // Quote Defense 2: Reject massive spreads (e.g., > 5%)
        double spread_pct = (quote->ask_price - quote->bid_price) / quote->ask_price;
        return spread_pct < 0.05;
    } else if (const auto* midTick = std::get_if<MidPointTick>(&event)) {
        return midTick->mid_price > 0; 
    } else if (const auto* bar = std::get_if<BarUpdate>(&event)) {
        // Bar Defense: EOD close must be valid
        return bar->close > 0.0;
    }

    // Default reject for unknown event types
    return false;
}

void BasketMeanReversionStrategy::onMarketEvent(const MarketEvent& event) {
    // Firewall: Drop any corrupted or anomalous market data instantly
    if (!isValidMarketEvent(event)) {
        return; 
    }

    if (const auto* trade = std::get_if<TradeTick>(&event)) {
        // processTick(trade->symbol, trade->price, trade->timestamp_ms);
    } else if (const auto* quote = std::get_if<QuoteTick>(&event)) {
        // double mid_price = (quote->bid_price + quote->ask_price) * 0.5;
        // processTick(quote->symbol, mid_price, quote->timestamp_ms);
    } else if (const auto* midTick = std::get_if<MidPointTick>(&event)) {
        processTick(midTick->symbol, midTick->mid_price, midTick->timestamp_ms);
    }
    else if (const auto* bar = std::get_if<BarUpdate>(&event)) {
        basket_state_->updateDailyClose(bar->symbol, bar->close);
    }
}

inline void BasketMeanReversionStrategy::processTick(const std::string& symbol, double price, uint64_t timestamp) {
    basket_state_->updatePrice(symbol, price, timestamp);

    if (!basket_state_->isWarmedUp()) return;

    // Get the optional Z-Score
    std::optional<double> opt_z_score = basket_state_->getZScore();
    
    // If prices are stale or variance is too low, skip this evaluation cycle entirely
    if (!opt_z_score.has_value()) {
        return; 
    }

    double z_score = opt_z_score.value();
    logZScoreThrottled(z_score, timestamp);

    if (master_state_ == MasterState::FLAT) {
        if (z_score > entry_z_score_) {
            LOG_DEBUG("[{}] Z-Score ({:.4f}) breached SHORT threshold ({:.4f}).", basket_id_, z_score, entry_z_score_);
            current_position_ = PositionState::SHORT;
            master_state_ = MasterState::ENTRY_PENDING;
            executeBasket(PositionState::SHORT);
        } else if (z_score < -entry_z_score_) {
            LOG_DEBUG("[{}] Z-Score ({:.4f}) breached LONG threshold ({:.4f}).", basket_id_, z_score, -entry_z_score_);
            current_position_ = PositionState::LONG;
            master_state_ = MasterState::ENTRY_PENDING;
            executeBasket(PositionState::LONG);
        }
    } 
    else if (master_state_ == MasterState::IN_POSITION) {
        // We know we are fully filled. What side are we on?
        if (current_position_ == PositionState::SHORT && z_score < exit_z_score_) {
            LOG_DEBUG("[{}] Z-Score ({:.4f}) breached FLAT threshold ({:.4f}). Exiting SHORT.", basket_id_, z_score, exit_z_score_);
            master_state_ = MasterState::EXIT_PENDING;
            executeBasket(PositionState::FLAT);
        } else if (current_position_ == PositionState::LONG && z_score > -exit_z_score_) {
            LOG_DEBUG("[{}] Z-Score ({:.4f}) breached FLAT threshold ({:.4f}). Exiting LONG.", basket_id_, z_score, -exit_z_score_);
            master_state_ = MasterState::EXIT_PENDING;
            executeBasket(PositionState::FLAT);
        }
    } 
    else{
        // Flying blind: Ignore all market ticks while waiting for exchange to resolve the inflight basket cycle
        return;
    }
}

void BasketMeanReversionStrategy::executeBasket(PositionState targetState) {
    size_t num_components = basket_state_->getNumComponents();
    std::vector<LegTarget> targets;

    // 1. Ask the Sizer for the exact mathematical execution targets
    if (targetState == PositionState::FLAT) {
        targets = BasketSizer::calculateExit(position_keys_, *portfolio_mngr_);
    } else {
        std::vector<double> current_prices(num_components);
        for (size_t i = 0; i < num_components; ++i) {
            current_prices[i] = basket_state_->getPrice(i);
        }

        auto opt_targets = BasketSizer::calculateEntry(
            targetState, capital_allocation_, basket_state_->getWeights(), current_prices);
            
        if (!opt_targets) {
            LOG_DEBUG("[{}] BasketSizer rejected entry. Reverting state to FLAT.", basket_id_);
            master_state_ = MasterState::FLAT;
            current_position_ = PositionState::FLAT;
            return;
        }
        targets = std::move(*opt_targets);
    }

    // 2. Build the cycle and route the orders
    dispatchCycle(targets, targetState == PositionState::FLAT ? "EXIT" : "ENTRY");

    UiBroadcaster::getInstance().broadcastState(basket_id_, 
        std::string(magic_enum::enum_name(master_state_)),
        std::string(magic_enum::enum_name(current_position_)));
}

// Starts a new execution cycle: one MARKET IOC order per non-zero leg, each with a fresh
// client order id so late reports from earlier cycles can never be attributed to this one.
void BasketMeanReversionStrategy::dispatchCycle(const std::vector<LegTarget>& targets, const char* label) {
    const auto& symbols = basket_state_->getSymbols();
    current_cycle_ = std::make_unique<ExecutionCycle>(basket_id_);

    std::vector<OrderRequest> orders;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].quantity <= 0.0) continue;
        OrderRequest req;
        req.client_order_id = OrderIdUtils::newClientOrderId(position_keys_[i]);
        req.symbol = symbols[i];
        req.side = targets[i].side;
        req.type = OrderType::MARKET;
        req.quantity = targets[i].quantity;
        req.tif = TimeInForce::IOC;
        current_cycle_->addLeg(req.client_order_id, req.symbol, req.quantity, req.side);
        orders.push_back(std::move(req));
    }

    // Every leg is registered before the first order goes out, so a fast fill can't
    // complete the cycle early.
    for (const auto& req : orders) {
        LOG_INFO("[{}] {} Routing: {} {} {:.4f} shares (MARKET IOC).", basket_id_, label,
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

void BasketMeanReversionStrategy::executeRecoverySweep() {
    LOG_WARN("[{}] Initiating Recovery Protocol: Sweeping residual inventory.", basket_id_);
    
    // State first: dispatchCycle may resolve the sweep synchronously.
    master_state_ = MasterState::RECOVERY_PENDING;
    dispatchCycle(BasketSizer::calculateExit(position_keys_, *portfolio_mngr_), "RECOVERY");
}

void BasketMeanReversionStrategy::onExecutionReport(const ExecutionReport& report) {
    if (!current_cycle_) return; // No active cycle

    // Let the cycle process the report. If it returns true, the cycle is finished.
    if (current_cycle_->processExecutionReport(report)) {
        evaluateCompletedCycle();
    }
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
        current_cycle_.reset();
        UiBroadcaster::getInstance().broadcastState(basket_id_, 
            std::string(magic_enum::enum_name(master_state_)),
            std::string(magic_enum::enum_name(current_position_)));
        return;
    }

    bool has_fills = current_cycle_->hasAnyFills();
    double missed_shares = current_cycle_->getMissedShares();

    // Scenario A: Perfect Execution
    if (has_fills && missed_shares <= 1e-6) {
        if (master_state_ == MasterState::ENTRY_PENDING) {
            master_state_ = MasterState::IN_POSITION;
            LOG_INFO("[{}] Entry cycle perfect. State -> IN_POSITION.", basket_id_);
        } else if (master_state_ == MasterState::EXIT_PENDING || master_state_ == MasterState::RECOVERY_PENDING) {
            master_state_ = MasterState::FLAT;
            current_position_ = PositionState::FLAT;
            LOG_INFO("[{}] Exit/Recovery cycle perfect. State -> FLAT.", basket_id_);
        }
        current_cycle_.reset();
    } 
    // Scenario B: Complete Whiff
    else if (!has_fills) {
        if (master_state_ == MasterState::ENTRY_PENDING) {
            LOG_WARN("[{}] Entry completely whiffed. Zero fills. Reverting State -> FLAT.", basket_id_);
            master_state_ = MasterState::FLAT;
            current_position_ = PositionState::FLAT;
        } else if (master_state_ == MasterState::EXIT_PENDING) {
            LOG_WARN("[{}] Exit completely whiffed. Zero fills. Reverting State -> IN_POSITION.", basket_id_);
            master_state_ = MasterState::IN_POSITION;
        } else if (master_state_ == MasterState::RECOVERY_PENDING) {
            LOG_ERROR("[{}] RECOVERY SWEEP WHIFFED! Exchange rejected market orders. Halting basket.", basket_id_);
            master_state_ = MasterState::SYSTEM_HALTED; // Stop trading forever, require manual human intervention
        }
        current_cycle_.reset();
    } 
    // Scenario C & D: Toxic Partial Fill (Basket unhedged)
    else if (has_fills && missed_shares > 1e-6) {
        if (master_state_ == MasterState::RECOVERY_PENDING) {
            LOG_ERROR("[{}] RECOVERY SWEEP PARTIALLY FILLED! Liquidity zero. Halting basket.", basket_id_);
            master_state_ = MasterState::SYSTEM_HALTED;
            current_cycle_.reset();
        } else {
            LOG_ERROR("[{}] Toxic partial fill! Basket broken. Missed shares: {:.4f}. Aborting and initiating Recovery Protocol.", basket_id_, missed_shares);
            executeRecoverySweep();
        }
    }

    UiBroadcaster::getInstance().broadcastState(basket_id_, 
        std::string(magic_enum::enum_name(master_state_)),
        std::string(magic_enum::enum_name(current_position_)));
}

bool BasketMeanReversionStrategy::hasInventory() const {
    for (const auto& key : position_keys_) {
        if (std::abs(portfolio_mngr_->getNetPosition(key)) > 1e-9) return true;
    }
    return false;
}

// Polls until the in-flight cycle resolves. NOTE: reports are processed on the event
// dispatcher thread while this runs on the caller's; Phase 5 moves shutdown onto the
// dispatcher to remove that race.
void BasketMeanReversionStrategy::waitForCycle(std::chrono::milliseconds timeout) const {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (current_cycle_ != nullptr && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void BasketMeanReversionStrategy::stop() {
    // 1. Orders still working: cancel them and let the cycle resolve. A partially filled
    //    entry resolves into a recovery sweep, which this also waits for.
    if (current_cycle_) {
        for (const auto& cid : current_cycle_->workingClientOrderIds()) {
            order_mngr_->cancelOrder(cid);
        }
        waitForCycle(kShutdownTimeout);
    }

    // 2. Flatten whatever inventory is still held.
    if (!current_cycle_ && hasInventory()) {
        master_state_ = MasterState::EXIT_PENDING;
        executeBasket(PositionState::FLAT);
        waitForCycle(kShutdownTimeout);
    }

    if (current_cycle_ != nullptr) {
        LOG_ERROR("[{}] Network timeout! Cycle did not resolve.", basket_id_);
    } else if (hasInventory()) {
        LOG_WARN("[{}] Shutdown finished but inventory remains. Orphaned positions may remain at the broker.", basket_id_);
    } else {
        LOG_INFO("[{}] Flat at shutdown.", basket_id_);
    }

    const auto& symbols = basket_state_->getSymbols();
    for (const auto& sym : symbols) {
        event_bus_->unsubscribeFromMarketEvents(sym, market_subid_[sym]);
    }
    event_bus_->unsubscribeFromExecution(report_subid_);
}

inline void BasketMeanReversionStrategy::logZScoreThrottled(double z_score, uint64_t timestamp) {
    if (timestamp - last_z_log_time_ms_ >= 500) {
        LOG_DEBUG("[{}] Current Z-Score: {:.4f}", basket_id_, z_score);
        last_z_log_time_ms_ = timestamp;
        UiBroadcaster::getInstance().broadcastZTick(basket_id_, z_score);
    }
}

} // namespace bot