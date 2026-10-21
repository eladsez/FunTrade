#include <risk/PortfolioManager.hpp>
#include <execution/IExecutionEngine.hpp>
#include <core/Logger.hpp>
#include <risk/UiBroadcaster.hpp>
#include <nlohmann/json.hpp>
#include <utils/StateStore.hpp>
#include <utils/Journal.hpp>
#include <algorithm>
#include <cmath>

namespace bot {

PortfolioManager::PortfolioManager(std::shared_ptr<IEventBus> eventBus, std::shared_ptr<IExecutionEngine> engine,
                                   std::string stateFile)
    : eventBus_(std::move(eventBus)), engine_(std::move(engine)), state_file_(std::move(stateFile)) {

    // 0. Restore the bot's own books from the last run
    loadState();
    
    // 1. Subscribe to all executions immediately
    eventBus_->subscribeToExecution([this](const ExecutionReport& report) {
        this->onExecution(report);
    });

    // 2. Use the wildcard to listen to ALL market data automatically.
    eventBus_->subscribeToMarketEvents("*", [this](const MarketEvent& event) {
        this->onMarketEvent(event);
    });
    
    // 3. Snapshot the broker's positions to reconcile against. Silent: live, this runs before
    // the broker connection exists (an empty snapshot would report every leg as a mismatch);
    // main calls resync() once the account is synced, which logs the real report.
    syncBrokerPositions();
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

        // Daily reconciliation: the first EOD bar of a new day arrives after that day's
        // closing-auction fills, so the broker and the virtual books should agree.
        if constexpr (std::is_same_v<T, BarUpdate>) {
            int64_t day = tradingDay(tick.timestamp_ms);
            bool due = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (day > last_reconcile_day_) { last_reconcile_day_ = day; due = true; }
            }
            if (due) {
                syncBrokerPositions();
                logReconciliation("Daily");
            }
        }

        // Only update if we derived a valid price
        if (updatedPrice > 0.0) {
            std::lock_guard<std::mutex> lock(mtx_);
            live_priced_.insert(symbol);
            auto it = symbol_to_vkeys_.find(symbol);
            if (it != symbol_to_vkeys_.end()) {
                for (const std::string& v_key : it->second) {
                    Position &pos = positions_[v_key];
                    pos.current_price = updatedPrice;
                    UiBroadcaster::getInstance().broadcastBasketPnL(v_key, pos.realized_pnl, pos.unrealized_pnl());
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

// Manages the O(1) symbol index; closed positions leave the index. Returns true if closed.
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
        
        // Keep the (now flat) entry: it carries the basket's realized PnL.
        return true;
    }
    
    return false; // Position merely modified in size, not closed
}

void PortfolioManager::onExecution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mtx_);

    // Only fills move inventory. Status-only reports (NEW, CANCELED, REJECTED, and IBKR's
    // orderStatus updates) carry last_quantity == 0, whatever their cumulative quantity says.
    if (report.last_quantity <= 0.0) {
        return;
    }

    // The same execution can be delivered more than once (IBKR re-sends after reconnects).
    if (!report.exec_id.empty() && !seen_exec_ids_.insert(report.exec_id).second) {
        LOG_DEBUG("[PortfolioManager] Ignoring duplicate execution {}", report.exec_id);
        return;
    }

    total_trades_++;

    std::string virtual_key = report.client_order_id.empty()
        ? report.symbol
        : OrderIdUtils::positionKeyOf(report.client_order_id);
    
    Position& pos = positions_[virtual_key];
    pos.symbol = report.symbol;
    pos.current_price = report.last_price;

    double signed_fill_qty = (report.side == Side::BUY) ? report.last_quantity : -report.last_quantity;
    bool was_flat = (std::abs(pos.quantity) < 1e-9);

    // 1. Calculate and apply the math
    double realized_this_trade = applyFill(pos, signed_fill_qty, report.last_price);
    pos.realized_pnl += realized_this_trade;
    total_realized_pnl_ += realized_this_trade;

    // we must broadcast it here because updatePositionIndex might erase it
    UiBroadcaster::getInstance().broadcastBasketPnL(virtual_key, pos.realized_pnl, pos.unrealized_pnl());

    bool is_flat_now = (std::abs(pos.quantity) < 1e-9);

    // 2. Synchronize memory and indices
    bool was_purged = updatePositionIndex(report.symbol, virtual_key, was_flat, is_flat_now);
    saveStateLocked();

    Journal::instance().record("fill", {{"basket", virtual_key.substr(0, virtual_key.rfind('_'))},
        {"position_key", virtual_key}, {"symbol", report.symbol},
        {"side", report.side == Side::BUY ? "BUY" : "SELL"}, {"quantity", report.last_quantity},
        {"price", report.last_price}, {"exec_id", report.exec_id}, {"client_order_id", report.client_order_id},
        {"position_after", pos.quantity}, {"realized_pnl", pos.realized_pnl}});

    // 3. Log results safely
    if (was_purged) {
        LOG_INFO("[PortfolioManager] Position closed for {} (realized {:.2f})", virtual_key, pos.realized_pnl);
        return; 
    }

    LOG_INFO("[PortfolioManager] Updated {} position: Symbol={} Qty={}, AvgPrice={}, RealizedPnL={}", 
             virtual_key, pos.symbol, pos.quantity, pos.avg_price, pos.realized_pnl);
}

void PortfolioManager::syncBrokerPositions() {
    std::vector<Position> brokerPos = engine_->getOpenPositions();
    std::map<std::string, double> fresh;
    for (const auto& p : brokerPos) {
        if (std::abs(p.quantity) > 1e-9) fresh[p.symbol] += p.quantity;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    broker_positions_.swap(fresh);
}

void PortfolioManager::resync() {
    syncBrokerPositions();
    logReconciliation("Startup");
}

double PortfolioManager::virtualQtyLocked(const std::string& symbol) const {
    double qty = 0.0;
    auto it = symbol_to_vkeys_.find(symbol);
    if (it == symbol_to_vkeys_.end()) return 0.0;
    for (const auto& vkey : it->second) {
        auto p = positions_.find(vkey);
        if (p != positions_.end()) qty += p->second.quantity;
    }
    return qty;
}

std::vector<SymbolMismatch> PortfolioManager::reconcileLocked() const {
    std::set<std::string> symbols;
    for (const auto& [sym, q] : broker_positions_) symbols.insert(sym);
    for (const auto& [sym, keys] : symbol_to_vkeys_) symbols.insert(sym);

    std::vector<SymbolMismatch> out;
    for (const auto& sym : symbols) {
        double v = virtualQtyLocked(sym);
        auto b = broker_positions_.find(sym);
        double bq = b == broker_positions_.end() ? 0.0 : b->second;
        if (std::abs(v - bq) > 1e-6) out.push_back({sym, v, bq});
    }
    return out;
}

std::vector<SymbolMismatch> PortfolioManager::reconcile() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return reconcileLocked();
}

bool PortfolioManager::isReconciled(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto b = broker_positions_.find(symbol);
    double bq = b == broker_positions_.end() ? 0.0 : b->second;
    return std::abs(virtualQtyLocked(symbol) - bq) <= 1e-6;
}

double PortfolioManager::getBrokerPosition(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto b = broker_positions_.find(symbol);
    return b == broker_positions_.end() ? 0.0 : b->second;
}

void PortfolioManager::logReconciliation(const char* when) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto mismatches = reconcileLocked();
    LOG_INFO("[PortfolioManager] === {} reconciliation: {} virtual positions, {} broker symbols, {} mismatches ===",
             when, positions_.size(), broker_positions_.size(), mismatches.size());
    for (const auto& [vkey, pos] : positions_) {
        LOG_INFO("  -> {} | {} {:.4f} @ {:.2f} | rPnL {:.2f} uPnL {:.2f}",
                 vkey, pos.symbol, pos.quantity, pos.avg_price, pos.realized_pnl, pos.unrealized_pnl());
    }
    for (const auto& m : mismatches) {
        LOG_ERROR("[PortfolioManager] MISMATCH {}: bot books {:.4f}, broker holds {:.4f}. Baskets trading {} will halt.",
                  m.symbol, m.virtual_qty, m.broker_qty, m.symbol);
    }
}

void PortfolioManager::seedPrice(const std::string& symbol, double price) {
    if (price <= 0.0) return;
    std::lock_guard<std::mutex> lock(mtx_);
    if (live_priced_.count(symbol)) return;
    auto it = symbol_to_vkeys_.find(symbol);
    if (it == symbol_to_vkeys_.end()) return;
    for (const auto& vkey : it->second) positions_[vkey].current_price = price;
}

void PortfolioManager::loadState() {
    if (state_file_.empty()) return;
    auto j = state::readJson(state_file_);  // corrupt file throws: never trade on unknown books
    if (!j) {
        LOG_INFO("[PortfolioManager] No state file at {}; starting with empty books.", state_file_);
        return;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& [vkey, p] : (*j)["positions"].items()) {
        Position pos;
        pos.symbol = p.at("symbol").get<std::string>();
        pos.quantity = p.at("quantity").get<double>();
        pos.avg_price = p.at("avg_price").get<double>();
        pos.current_price = pos.avg_price;
        pos.realized_pnl = p.value("realized_pnl", 0.0);
        positions_[vkey] = pos;
        symbol_to_vkeys_[pos.symbol].push_back(vkey);
    }
    for (const auto& id : (*j)["seen_exec_ids"]) seen_exec_ids_.insert(id.get<std::string>());
    total_realized_pnl_ = j->value("total_realized_pnl", 0.0);
    LOG_INFO("[PortfolioManager] Restored {} virtual positions and {} execution ids from {}.",
             positions_.size(), seen_exec_ids_.size(), state_file_);
}

void PortfolioManager::saveStateLocked() const {
    if (state_file_.empty()) return;
    nlohmann::json j;
    j["positions"] = nlohmann::json::object();
    for (const auto& [vkey, pos] : positions_) {
        j["positions"][vkey] = {{"symbol", pos.symbol}, {"quantity", pos.quantity},
                                {"avg_price", pos.avg_price}, {"realized_pnl", pos.realized_pnl}};
    }
    j["seen_exec_ids"] = seen_exec_ids_;
    j["total_realized_pnl"] = total_realized_pnl_;
    state::writeJsonAtomic(state_file_, j);
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

double PortfolioManager::getPnL(const std::vector<std::string>& virtual_keys) const {
    std::lock_guard<std::mutex> lock(mtx_);
    double pnl = 0.0;
    for (const auto& k : virtual_keys) {
        auto it = positions_.find(k);
        if (it != positions_.end()) pnl += it->second.realized_pnl + it->second.unrealized_pnl();
    }
    return pnl;
}

double PortfolioManager::getGrossExposure() const {
    std::lock_guard<std::mutex> lock(mtx_);
    double gross = 0.0;
    for (const auto& [k, p] : positions_) gross += std::abs(p.quantity * p.current_price);
    return gross;
}

double PortfolioManager::getNetSymbolQuantity(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return virtualQtyLocked(symbol);
}

std::map<std::string, PortfolioManager::SymbolNet> PortfolioManager::getSymbolNets() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::map<std::string, SymbolNet> out;
    for (const auto& [sym, vkeys] : symbol_to_vkeys_) {
        SymbolNet n;
        for (const auto& vkey : vkeys) {
            auto p = positions_.find(vkey);
            if (p == positions_.end()) continue;
            n.quantity += p->second.quantity;
            if (p->second.current_price > 0.0) n.price = p->second.current_price;
        }
        if (std::abs(n.quantity) > 1e-9) out.emplace(sym, n);
    }
    return out;
}

size_t PortfolioManager::getTotalTrades() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return total_trades_;
}

} // namespace bot
