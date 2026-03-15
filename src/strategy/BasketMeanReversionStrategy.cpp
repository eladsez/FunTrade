#include <strategy/BasketMeanReversionStrategy.hpp>
#include <core/Logger.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace bot {

BasketMeanReversionStrategy::BasketMeanReversionStrategy(
    std::shared_ptr<IEventBus> eventBus,
    std::shared_ptr<IExecutionEngine> executionEngine,
    std::shared_ptr<PortfolioManager> portfolioMngr,
    const std::string& configFilePath,
    double capitalAllocation)
    : event_bus_(std::move(eventBus)),
      exec_engine_(std::move(executionEngine)),
      portfolio_mngr_(std::move(portfolioMngr)),
      config_path_(configFilePath),
      capital_allocation_(capitalAllocation) {}

void BasketMeanReversionStrategy::start() {
    loadConfig();
    warmUp();

    // 1. Subscribe to real-time events for our basket components
    const auto& symbols = basket_state_->getSymbols();
    for (const auto& sym : symbols) {
        event_bus_->subscribeToMarketEvents(
            sym, 
            [this](const MarketEvent& e) { this->onMarketEvent(e); }
        );
    }
    // 2. Listen for execution reports to confirm our orders
    event_bus_->subscribeToExecution(
        [this](const ExecutionReport& report) { this->onExecutionReport(report); }
    );
}

void BasketMeanReversionStrategy::loadConfig() {
    std::ifstream file(config_path_);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + config_path_);
    }

    nlohmann::json config;
    file >> config;

    // Parse nested structure from the Python export
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
        
        client_order_ids_.push_back(config["basket_id"].get<std::string>() + "_" + sym);
    }

    basket_state_ = std::make_unique<BasketState>(window_size, symbols, weights);
}

// Helper function to generate RFC3339 / ISO8601 timestamps
static std::string formatTimePoint(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val;
    
    // Thread-safe GMT time conversion
#if defined(_WIN32)
    gmtime_s(&tm_val, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm_val);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
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

    std::string end_time = formatTimePoint(now);
    std::string start_time = formatTimePoint(start);

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
}

void BasketMeanReversionStrategy::onMarketEvent(const MarketEvent& event) {
    if (const auto* trade = std::get_if<TradeTick>(&event)) {
        processTick(trade->symbol, trade->price, trade->timestamp_ms);
    } else if (const auto* quote = std::get_if<QuoteTick>(&event)) {
        double mid_price = (quote->bid_price + quote->ask_price) * 0.5;
        processTick(quote->symbol, mid_price, quote->timestamp_ms);
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

    if (current_position_ == PositionState::FLAT) {
        if (z_score > entry_z_score_) {
            current_position_ = PositionState::PENDING_SHORT;
            executeBasket(PositionState::SHORT);
        } else if (z_score < -entry_z_score_) {
            current_position_ = PositionState::PENDING_LONG;
            executeBasket(PositionState::LONG);
        }
    } else if (current_position_ == PositionState::SHORT) {
        if (z_score < exit_z_score_) {
            current_position_ = PositionState::PENDING_FLAT;  // Lock it
            executeBasket(PositionState::FLAT);
        }
    } else if (current_position_ == PositionState::LONG) {
        if (z_score > -exit_z_score_) {
            current_position_ = PositionState::PENDING_FLAT;  // Lock it
            executeBasket(PositionState::FLAT);
        }
    }
}

void BasketMeanReversionStrategy::executeBasket(PositionState targetState) {
    size_t num_components = basket_state_->getNumComponents();
    const auto& symbols = basket_state_->getSymbols();
    const auto& weights = basket_state_->getWeights();

    for (size_t i = 0; i < num_components; ++i) {
        double weight = weights[i];
        Side order_side;
        double target_quantity = 0.0;

        if (targetState == PositionState::FLAT) {
            // Retrieve the exact virtual position we currently hold for this specific basket leg
            Position current_pos = portfolio_manager_->getNetPosition(client_order_ids_[i]);
            // If it's 0.0 (either doesn't exist or already flat), skip it
            if (std::abs(net_qty) < 1e-9) continue; 

            // The absolute value is exactly how many shares we need to order
            target_quantity = std::abs(net_qty);

            // Reverse the side based on the sign to close it out
            order_side = (net_qty > 0) ? Side::SELL : Side::BUY;
        } else {
            // We are entering a new position. Calculate quantity based on capital allocation.
            double absolute_weight = std::abs(weight);
            double current_price = basket_state_->getPrice(i);
            
            if (current_price <= 0) continue; 
            
            target_quantity = (capital_allocation_ * absolute_weight) / current_price;

            if (targetState == PositionState::LONG) {
                order_side = (weight > 0) ? Side::BUY : Side::SELL;
            } else { // SHORT
                order_side = (weight > 0) ? Side::SELL : Side::BUY;
            }
        }

        OrderRequest req;
        req.client_order_id = client_order_ids_[i]; 
        req.symbol = symbols[i];
        req.side = order_side;
        req.type = OrderType::MARKET;
        req.tif = TimeInForce::IOC;
        req.quantity = target_quantity;

        exec_engine_->placeOrder(req);
    }
}

void BasketMeanReversionStrategy::onExecutionReport(const ExecutionReport& report) {
    // 1. Check if this report belongs to THIS specific basket
    bool is_our_order = false;
    for (const auto& cid : client_order_ids_) {
        // Clean, readable, and centralized
        if (OrderIdUtils::isMatchingId(cid, report.client_order_id)) {
            is_our_order = true;
            break;
        }
    }
    if (!is_our_order) return;

    // 2. State Machine Reconciliation
    if (report.status == OrderStatus::FILLED) {
        // Note: For a true basket, you'd want to count fills and ensure ALL legs 
        // are filled before unlocking, but for simplicity, we transition on the first successful leg fill.
        
        if (current_position_ == PositionState::PENDING_LONG) {
            current_position_ = PositionState::LONG;
            LOG_INFO("[{}] Successfully entered LONG basket.", config_path_);
        } else if (current_position_ == PositionState::PENDING_SHORT) {
            current_position_ = PositionState::SHORT;
            LOG_INFO("[{}] Successfully entered SHORT basket.", config_path_);
        } else if (current_position_ == PositionState::PENDING_FLAT) {
            current_position_ = PositionState::FLAT;
            LOG_INFO("[{}] Successfully FLATTENED basket.", config_path_);
        }
    } else if (report.status == OrderStatus::REJECTED || report.status == OrderStatus::CANCELED) {
        // FATAL: The exchange rejected our order. We must revert the state lock!
        LOG_ERROR("[{}] Order rejected! Reverting state machine.", config_path_);
        
        if (current_position_ == PositionState::PENDING_LONG || current_position_ == PositionState::PENDING_SHORT) {
            current_position_ = PositionState::FLAT; 
        } else if (current_position_ == PositionState::PENDING_FLAT) {
            // We failed to close. We are technically still in the previous position.
            // (You would need logic here to determine if you were LONG or SHORT previously)
        }
    }
}

void BasketMeanReversionStrategy::stop() {
    if (current_position_ != PositionState::FLAT) {
        executeBasket(PositionState::FLAT);
    }
    exec_engine_->cancelAllOrders();
}

} // namespace bot