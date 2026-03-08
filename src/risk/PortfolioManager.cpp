#include <risk/PortfolioManager.hpp>
#include <execution/IExecutionEngine.hpp>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <cmath>

namespace bot {

PortfolioManager::PortfolioManager(IEventBus& eventBus) : eventBus_(eventBus) {
    // PortfolioManager should subscribe to all executions
    eventBus_.subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });
    
    // We don't subscribe to ticks here globally because we don't know which symbols yet.
    // Or we can subscribe as symbols are added to the portfolio.
    // For simplicity in this OBI bot, we'll assume we know the symbols or subscribe on first execution.
}

void PortfolioManager::onTick(const MarketTick& tick) {
    std::string symbol = tick.symbol;
    symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());
    
    std::lock_guard<std::mutex> lock(mtx_);
    if (positions_.count(symbol)) {
        positions_[symbol].current_price = tick.price;
    }
}

void PortfolioManager::onExecution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // We only care about fills
    if (report.status != OrderStatus::FILLED && report.status != OrderStatus::PARTIALLY_FILLED) {
        return;
    }

    std::string symbol = report.symbol;
    symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());

    Position& pos = positions_[symbol];
    pos.symbol = symbol;
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

void PortfolioManager::syncWithExchange(IExecutionEngine& engine) {
    LOG_INFO("[PortfolioManager] Syncing positions with exchange...");
    nlohmann::json positionsJson = engine.getOpenPositions();
    
    std::lock_guard<std::mutex> lock(mtx_);
    positions_.clear();
    
    if (positionsJson.is_array()) {
        for (const auto& posJson : positionsJson) {
            try {
                std::string symbol = posJson.value("symbol", "");
                symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());
                
                Position pos;
                pos.symbol = symbol;
                pos.quantity = std::stod(posJson.value("qty", "0"));
                pos.avg_price = std::stod(posJson.value("avg_entry_price", "0"));
                pos.current_price = std::stod(posJson.value("current_price", "0"));
                
                positions_[symbol] = pos;
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

double PortfolioManager::getNetPosition(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = positions_.find(symbol);
    return (it != positions_.end()) ? it->second.quantity : 0.0;
}

Position PortfolioManager::getPosition(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = positions_.find(symbol);
    if (it != positions_.end()) return it->second;
    return {symbol, 0.0, 0.0, 0.0, 0.0};
}

} // namespace bot
