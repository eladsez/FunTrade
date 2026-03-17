#include <risk/PortfolioManager.hpp>
#include <execution/IExecutionEngine.hpp>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <cmath>

namespace bot {

PortfolioManager::PortfolioManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine) 
    : eventBus_(std::move(eventBus)), engine_(std::move(engine)) {
    
    // 1. Subscribe to all executions immediately
    eventBus_->subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });
    
    // 2. Automatically sync with the exchange on startup
    syncWithExchange();
}

// void PortfolioManager::ensureMarketSubscription(const std::string& symbol) {
//     // Check if we are already subscribed to this symbol's tick data
//     if (subscribedSymbols_.find(symbol) == subscribedSymbols_.end()) {
//         subscribedSymbols_.insert(symbol);
        
//         LOG_INFO("[PortfolioManager] Auto-subscribing to market ticks for {}", symbol);
        
//         eventBus_->subscribeToMarketEvents(symbol, [this](const MarketEvent& event) {
//             this->onMarketEvent(event);
//         });
//     }
// }

void PortfolioManager::onMarketEvent(const MarketEvent& event) {
    std::visit([this](auto&& tick) {
        using T = std::decay_t<decltype(tick)>;
        
        std::string symbol = tick.symbol;
        double updatedPrice = 0.0;

        if constexpr (std::is_same_v<T, TradeTick>) {
            // Use the exact last traded price
            updatedPrice = tick.price;
        } 
        else if constexpr (std::is_same_v<T, QuoteTick>) {
            // Mark-to-Market using the mid-price of the spread
            if (tick.bid_price > 0 && tick.ask_price > 0) {
                updatedPrice = (tick.bid_price + tick.ask_price) / 2.0;
            }
        } 
        else if constexpr (std::is_same_v<T, OrderBookUpdate>) {
            // Ignore ORDERBOOK ticks for PnL to save CPU, Quotes are enough
            return; 
        }

        // Only update if we derived a valid price
        if (updatedPrice > 0.0) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (positions_.count(symbol)) {
                LOG_DEBUG("[PortfolioManager] Update price for {}:{:.2f} -> {:.2f}", 
                    symbol, 
                    positions_[symbol].current_price, 
                    updatedPrice);
                positions_[symbol].current_price = updatedPrice;
            }
        }
    }, event);
}

void PortfolioManager::onExecution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (report.status != OrderStatus::FILLED && report.status != OrderStatus::PARTIALLY_FILLED) {
        return;
    }

    // Ensure we are getting live price updates for this newly traded symbol
    // ensureMarketSubscription(symbol);

    // the point of the virtualkey here is to be able to track per baket positions
    // when there is more the one basket sharing the same symbol 
    std::string base_id = OrderIdUtils::extractBaseClientId(report.client_order_id);
    std::string virtual_key = base_id.empty() ? report.symbol : base_id;

    Position& pos = positions_[virtual_key];
    pos.symbol = report.symbol;
    pos.current_price = report.last_price; // Update price on fill

    double fill_qty = report.last_quantity;
    double fill_price = report.last_price;
    
    // Convert Side to signed quantity for easier calculation
    double signed_fill_qty = (report.side == Side::BUY) ? fill_qty : -fill_qty;

    if (std::abs(pos.quantity) < 1e-9) {
        // Opening new position
        pos.quantity = signed_fill_qty;
        pos.avg_price = fill_price;
    } else if ((pos.quantity > 0 && signed_fill_qty > 0) || (pos.quantity < 0 && signed_fill_qty < 0)) {
        // Increasing existing position (same side)
        double total_qty = pos.quantity + signed_fill_qty;
        pos.avg_price = (pos.quantity * pos.avg_price + signed_fill_qty * fill_price) / total_qty;
        pos.quantity = total_qty;
    } else {
        // Reducing or flipping position (opposite side)
        double reduction = std::min(std::abs(pos.quantity), std::abs(signed_fill_qty));
        double realized = 0.0;
        
        if (pos.quantity > 0) {
            // Long position being reduced/closed by a SELL
            realized = reduction * (fill_price - pos.avg_price);
        } else {
            // Short position being reduced/closed by a BUY
            realized = reduction * (pos.avg_price - fill_price);
        }
        
        pos.realized_pnl += realized;
        total_realized_pnl_ += realized;
        
        double remaining_qty = pos.quantity + signed_fill_qty;
        
        if ((pos.quantity > 0 && remaining_qty < -1e-9) || (pos.quantity < 0 && remaining_qty > 1e-9)) {
            // Position flipped
            pos.quantity = remaining_qty;
            pos.avg_price = fill_price;
        } else {
            // Position reduced but not flipped
            pos.quantity = remaining_qty;
            // avg_price remains the same for the remaining part
            if (std::abs(pos.quantity) < 1e-9) {
                pos.quantity = 0.0;
                pos.avg_price = 0.0;
            }
        }
    }

    LOG_INFO("[PortfolioManager] Updated {} position: Qty={}, AvgPrice={}, RealizedPnL={}", 
             pos.symbol, pos.quantity, pos.avg_price, pos.realized_pnl);
}

void PortfolioManager::syncWithExchange() {
    LOG_INFO("[PortfolioManager] Syncing positions with exchange...");
    nlohmann::json positionsJson = engine_->getOpenPositions();
    
    std::lock_guard<std::mutex> lock(mtx_);
    positions_.clear();
    
    if (positionsJson.is_array()) {
        for (const auto& posJson : positionsJson) {
            try {
                std::string symbol = posJson.value("symbol", "");
                
                Position pos;
                pos.symbol = symbol;
                pos.quantity = std::stod(posJson.value("qty", "0"));
                pos.avg_price = std::stod(posJson.value("avg_entry_price", "0"));
                pos.current_price = std::stod(posJson.value("current_price", "0"));
                
                positions_[symbol] = pos;
                
                // // Ensure we get live price updates for symbols we ALREADY own from a previous run
                // ensureMarketSubscription(symbol);
                
                LOG_INFO("[PortfolioManager] Synced {} | Qty: {} | AvgPrice: {}", symbol, pos.quantity, pos.avg_price);
            } catch (const std::exception& e) {
                LOG_ERROR("[PortfolioManager] Error parsing synced position: {}", e.what());
            }
        }
    }
    // --- LOG THE FINAL STATE AFTER SYNC ---
    LOG_INFO("[PortfolioManager] === Sync Complete. Current Portfolio State ===");
    LOG_INFO("[PortfolioManager] Total open positions: {}", positions_.size());
    
    for (const auto& [symbol, pos] : positions_) {
        // Calculate a quick snapshot of unrealized PnL for the log
        double unrealizedPnL = (pos.current_price - pos.avg_price) * pos.quantity;
        
        LOG_INFO("  -> Symbol: {} | Qty: {:.4f} | Avg Price: {:.2f} | Cur Price: {:.2f} | uPnL: {:.2f}", 
                 symbol, pos.quantity, pos.avg_price, pos.current_price, unrealizedPnL);
    }
    LOG_INFO("[PortfolioManager] ================================================");
}

double PortfolioManager::getTotalRealizedPnL() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return total_realized_pnl_;
}

double PortfolioManager::getTotalUnrealizedPnL() const {
    std::lock_guard<std::mutex> lock(mtx_);
    double total_unrealized = 0.0;
    for (const auto& [symbol, pos] : positions_) {
        total_unrealized += pos.unrealized_pnl();
    }
    return total_unrealized;
}

double PortfolioManager::getNetPosition(const std::string& virtual_key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = positions_.find(virtual_key);
    return (it != positions_.end()) ? it->second.quantity : 0.0;
}

std::optional<Position> PortfolioManager::getPosition(const std::string& virtual_key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = positions_.find(virtual_key);
    
    if (it != positions_.end()) {
        return it->second;
    } 

    return std::nullopt; // Safely indicate the position does not exist
}

std::optional<double> PortfolioManager::getPortfolioValue() const {
    return engine_->getPortfolioValue();
}

std::optional<double> PortfolioManager::getAvailableCash() const {
    // Note: Make sure getAvailableCash() is added to IExecutionEngine, 
    // AlpacaExecutionEngine, and MockExecutionEngine as discussed previously!
    return engine_->getAvailableCash();
}
} // namespace bot
