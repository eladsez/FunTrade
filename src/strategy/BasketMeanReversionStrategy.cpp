#include <strategy/BasketMeanReversionStrategy.hpp>
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
    std::shared_ptr<IDataFeed> dataFeed,
    std::shared_ptr<IExecutionEngine> executionEngine,
    const std::string& configFilePath)
    : event_bus_(std::move(eventBus)),
      data_feed_(std::move(dataFeed)),
      exec_engine_(std::move(executionEngine)),
      config_path_(configFilePath) {}

void BasketMeanReversionStrategy::start() {
    loadConfig();
    warmUp();

    // Subscribe to real-time events for our basket components
    const auto& symbols = basket_state_->getSymbols();
    for (const auto& sym : symbols) {
        data_feed_->subscribeToSymbol(sym);
        event_bus_->subscribeToMarketEvents(
            sym, 
            [this](const MarketEvent& e) { this->onMarketEvent(e); }
        );
    }
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

void BasketMeanReversionStrategy::warmUp() {
    // 1. Calculate dynamic timestamps based on current clock
    auto now = std::chrono::system_clock::now();
    
    // Request roughly 1.5x the window size in calendar days to account for weekends and market holidays.
    int calendar_days_back = basket_state_->getWindowSize() * 2; 
    auto start = now - std::chrono::hours(24 * calendar_days_back);

    std::string end_time = formatTimePoint(now);
    std::string start_time = formatTimePoint(start);

    size_t num_components = basket_state_->getNumComponents();
    const auto& symbols = basket_state_->getSymbols();
    const auto& weights = basket_state_->getWeights();

    std::vector<std::vector<HistoricalBar>> all_bars(num_components);
    size_t min_bars = std::numeric_limits<size_t>::max();

    for (size_t i = 0; i < num_components; ++i) {
        all_bars[i] = data_feed_->fetchHistoricalBars(symbols[i], start_time, end_time, "1Day");
        if (all_bars[i].size() < min_bars) {
            min_bars = all_bars[i].size();
        }
    }

    for (size_t t = 0; t < min_bars; ++t) {
        double historical_spread = 0.0;
        for (size_t i = 0; i < num_components; ++i) {
            historical_spread += all_bars[i][t].close * weights[i];
            basket_state_->updatePrice(symbols[i], all_bars[i][t].close, all_bars[i][t].timestamp_ms);
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
}

inline void BasketMeanReversionStrategy::processTick(const std::string& symbol, double price, uint64_t timestamp) {
    basket_state_->updatePrice(symbol, price, timestamp);

    if (!basket_state_->isWarmedUp()) return;

    double z_score = basket_state_->getZScore();

    if (current_position_ == PositionState::FLAT) {
        if (z_score > entry_z_score_) {
            executeBasket(PositionState::SHORT);
            current_position_ = PositionState::SHORT;
        } else if (z_score < -entry_z_score_) {
            executeBasket(PositionState::LONG);
            current_position_ = PositionState::LONG;
        }
    } else if (current_position_ == PositionState::SHORT) {
        if (z_score < exit_z_score_) {
            executeBasket(PositionState::FLAT);
            current_position_ = PositionState::FLAT;
        }
    } else if (current_position_ == PositionState::LONG) {
        if (z_score > -exit_z_score_) {
            executeBasket(PositionState::FLAT);
            current_position_ = PositionState::FLAT;
        }
    }
}

void BasketMeanReversionStrategy::onBarClose() {
    // Triggers the mathematical advancement of the rolling window.
    // Must be called externally by a clock or bar aggregator at the end of the timeframe.
    if (basket_state_) {
        basket_state_->advanceWindow();
    }
}

void BasketMeanReversionStrategy::executeBasket(PositionState targetState) {
    size_t num_components = basket_state_->getNumComponents();
    const auto& symbols = basket_state_->getSymbols();
    const auto& weights = basket_state_->getWeights();

    for (size_t i = 0; i < num_components; ++i) {
        double weight = weights[i];
        
        Side order_side;
        if (targetState == PositionState::LONG) {
            order_side = (weight > 0) ? Side::BUY : Side::SELL;
        } else if (targetState == PositionState::SHORT) {
            order_side = (weight > 0) ? Side::SELL : Side::BUY;
        } else {
            // Reversing the position to get FLAT
            order_side = (current_position_ == PositionState::LONG) 
                ? ((weight > 0) ? Side::SELL : Side::BUY)
                : ((weight > 0) ? Side::BUY : Side::SELL);
        }

        double absolute_weight = std::abs(weight);
        double current_price = basket_state_->getPrice(i);
        
        if (current_price <= 0) continue; 
        
        double quantity = (BASE_ALLOCATION * absolute_weight) / current_price;

        OrderRequest req;
        // Pre-allocated to avoid SSO allocation in hot path
        req.client_order_id = client_order_ids_[i]; 
        req.symbol = symbols[i];
        req.side = order_side;
        req.type = OrderType::MARKET;
        req.tif = TimeInForce::IOC;
        req.quantity = quantity;

        exec_engine_->placeOrder(req);
    }
}

void BasketMeanReversionStrategy::stop() {
    if (current_position_ != PositionState::FLAT) {
        executeBasket(PositionState::FLAT);
    }
    exec_engine_->cancelAllOrders();
}

} // namespace bot