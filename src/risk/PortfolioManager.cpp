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

    // 2. Use the wildcard to listen to ALL market data automatically.
    eventBus_->subscribeToMarketEvents("*", [this](const MarketEvent& event) {
        this->onMarketEvent(event);
    });
    
    // 3. Automatically sync with the exchange on startup
    syncWithExchange();
}


void PortfolioManager::onMarketEvent(const MarketEvent& event) {
    std::visit([this](auto&& tick) {
        using T = std::decay_t<decltype(tick)>;
        
        std::string symbol = tick.symbol;
        double updatedPrice = 0.0;

        if constexpr (std::is_same_v<T, TradeTick>) {
            updatedPrice = tick.price;
        } 
        else if constexpr (std::is_same_v<T, QuoteTick>) {
            if (tick.bid_price > 0 && tick.ask_price > 0) {
                updatedPrice = (tick.bid_price + tick.ask_price) / 2.0;
            }
        }
        else if constexpr (std::is_same_v<T, BarUpdate>) {
            // Make sure we capture EOD Bar updates too for backtesting!
            updatedPrice = tick.close;
        } 

        // Only update if we derived a valid price
        if (updatedPrice > 0.0) {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = symbol_to_vkeys_.find(symbol);
            if (it != symbol_to_vkeys_.end()) {
                for (const std::string& v_key : it->second) {
                    positions_[v_key].current_price = updatedPrice;
                }
            }
        }
    }, event);
}

// Handles VWAP, quantity updates, and returns the realized PnL of the fill
double PortfolioManager::applyFill(Position& pos, double signed_fill_qty, double fill_price) {
    if (std::abs(pos.quantity) < 1e-9) {
        // Opening new position
        pos.quantity = signed_fill_qty;
        pos.avg_price = fill_price;
        return 0.0;
    } 
    
    if ((pos.quantity > 0 && signed_fill_qty > 0) || (pos.quantity < 0 && signed_fill_qty < 0)) {
        // Increasing existing position
        double total_qty = pos.quantity + signed_fill_qty;
        pos.avg_price = (pos.quantity * pos.avg_price + signed_fill_qty * fill_price) / total_qty;
        pos.quantity = total_qty;
        return 0.0;
    } 
    
    // Reducing or flipping position
    double reduction = std::min(std::abs(pos.quantity), std::abs(signed_fill_qty));
    double realized = (pos.quantity > 0) 
        ? reduction * (fill_price - pos.avg_price)
        : reduction * (pos.avg_price - fill_price);
        
    double remaining_qty = pos.quantity + signed_fill_qty;
    
    if ((pos.quantity > 0 && remaining_qty < -1e-9) || (pos.quantity < 0 && remaining_qty > 1e-9)) {
        // Position flipped
        pos.quantity = remaining_qty;
        pos.avg_price = fill_price;
    } else {
        // Position reduced
        pos.quantity = remaining_qty;
        if (std::abs(pos.quantity) < 1e-9) {
            pos.quantity = 0.0;
            pos.avg_price = 0.0;
        }
    }
    
    return realized;
}

// Manages the O(1) index and purges closed positions. Returns true if purged.
bool PortfolioManager::updatePositionIndex(const std::string& symbol, const std::string& virtual_key, bool was_flat, bool is_flat_now) {
    if (was_flat && !is_flat_now) {
        // Position Opened: Add to O(1) index
        auto& vkeys = symbol_to_vkeys_[symbol];
        if (std::find(vkeys.begin(), vkeys.end(), virtual_key) == vkeys.end()) {
            vkeys.push_back(virtual_key);
        }
        return false;
    } 
    
    if (!was_flat && is_flat_now) {
        // Position Closed: Purge from memory
        auto& vkeys = symbol_to_vkeys_[symbol];
        vkeys.erase(std::remove(vkeys.begin(), vkeys.end(), virtual_key), vkeys.end());
        
        if (vkeys.empty()) {
            symbol_to_vkeys_.erase(symbol);
        }
        
        positions_.erase(virtual_key);
        return true;
    }
    
    return false; // Position merely modified in size, not closed
}

void PortfolioManager::onExecution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (report.status != OrderStatus::FILLED && report.status != OrderStatus::PARTIALLY_FILLED) {
        return;
    }

    if (report.status == OrderStatus::FILLED) {
        total_trades_++;
    }

    std::string virtual_key = report.client_order_id.empty() ? report.symbol : report.client_order_id;
    
    Position& pos = positions_[virtual_key];
    pos.symbol = report.symbol;
    pos.current_price = report.last_price;

    double signed_fill_qty = (report.side == Side::BUY) ? report.last_quantity : -report.last_quantity;
    bool was_flat = (std::abs(pos.quantity) < 1e-9);

    // 1. Calculate and apply the math
    double realized_this_trade = applyFill(pos, signed_fill_qty, report.last_price);
    pos.realized_pnl += realized_this_trade;
    total_realized_pnl_ += realized_this_trade;

    bool is_flat_now = (std::abs(pos.quantity) < 1e-9);

    // 2. Synchronize memory and indices
    bool was_purged = updatePositionIndex(report.symbol, virtual_key, was_flat, is_flat_now);

    // 3. Log results safely
    if (was_purged) {
        LOG_INFO("[PortfolioManager] Position closed and purged for: {}", virtual_key);
        return; 
    }

    LOG_INFO("[PortfolioManager] Updated {} position: Symbol={} Qty={}, AvgPrice={}, RealizedPnL={}", 
             virtual_key, pos.symbol, pos.quantity, pos.avg_price, pos.realized_pnl);
}

void PortfolioManager::syncWithExchange() {
    LOG_INFO("[PortfolioManager] Syncing positions with exchange...");
    std::vector<Position> openPos = engine_->getOpenPositions();
    
    // 1. Build the new map completely offline (no locks)
    std::unordered_map<std::string, Position> new_positions;
    new_positions.reserve(openPos.size());
     
    for (auto& pos : openPos) {
        // We need a copy of the symbol for the key, but we can move the struct itself
        std::string key = pos.symbol;
        
        // Log locally before we move 'pos' and lose its data
        LOG_DEBUG("[PortfolioManager] Synced {} | Qty: {} | AvgPrice: {}", 
                  pos.symbol, pos.quantity, pos.avg_price);

        // Build the secondary index. When pulling raw from the broker,
        // we don't have the client_order_id, so the v_key is just the symbol.
        new_symbol_to_vkeys[pos.symbol].push_back(key);
        new_positions.emplace(std::move(key), std::move(pos));
    }

    // --- LOG THE FINAL STATE BEFORE LOCKING ---
    LOG_INFO("[PortfolioManager] === Sync Complete. Current Portfolio State ===");
    LOG_INFO("[PortfolioManager] Total open positions: {}", new_positions.size());
    
    for (const auto& [symbol, pos] : new_positions) {
        LOG_INFO("  -> Symbol: {} | Qty: {:.4f} | Avg Price: {:.2f} | Cur Price: {:.2f} | uPnL: {:.2f}", 
                 symbol, pos.quantity, pos.avg_price, pos.current_price, pos.unrealized_pnl());
    }
    LOG_INFO("[PortfolioManager] ================================================");

    // 2. Lock the mutex ONLY for the swap. This is an O(1) operation.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        positions_.swap(new_positions);
        symbol_to_vkeys_.swap(new_symbol_to_vkeys);
    }
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
    return engine_->getAvailableCash();
}

size_t PortfolioManager::getTotalTrades() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return total_trades_;
}

} // namespace bot
