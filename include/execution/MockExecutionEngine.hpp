#pragma once

#include <execution/IExecutionEngine.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>
#include <core/Logger.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>

namespace bot {

// Simulated exchange for backtests.
//   * MARKET orders fill instantly at the latest price seen.
//   * MARKET_ON_CLOSE orders fill at the close of the session that is open when they arrive:
//     the day of the last intraday event seen, or the day after the last close bar seen.
//     This is the fill price the research simulator (compute.py) assumes.
// It also keeps broker-side positions so the PortfolioManager can reconcile against it.
// Use with DispatchMode::Synchronous for deterministic results.
class MockExecutionEngine : public IExecutionEngine {
public:
    explicit MockExecutionEngine(std::shared_ptr<IEventBus> eventBus)
        : event_bus_(std::move(eventBus)) {

        // Wildcard listeners run after symbol listeners, so strategies act on an event
        // before the exchange advances to it.
        event_bus_->subscribeToMarketEvents("*", [this](const MarketEvent& event) {
            std::visit([this](auto&& tick) {
                using T = std::decay_t<decltype(tick)>;
                std::lock_guard<std::mutex> lock(mtx_);
                if constexpr (std::is_same_v<T, TradeTick>) {
                    latest_prices_[tick.symbol] = tick.price;
                } else if constexpr (std::is_same_v<T, QuoteTick>) {
                    latest_prices_[tick.symbol] = (tick.bid_price + tick.ask_price) / 2.0;
                } else if constexpr (std::is_same_v<T, MidPointTick>) {
                    latest_prices_[tick.symbol] = tick.mid_price;
                } else if constexpr (std::is_same_v<T, BarUpdate>) {
                    latest_prices_[tick.symbol] = tick.close;
                    fillMocOrdersLocked(tick.symbol, tradingDay(tick.timestamp_ms), tick.close);
                }
                if (tick.timestamp_ms > 0) {
                    int64_t d = tradingDay(tick.timestamp_ms);
                    // After a close bar, the next order belongs to the next session.
                    int64_t session = std::is_same_v<T, BarUpdate> ? d + 1 : d;
                    open_session_ = std::max(open_session_, session);
                }
            }, event);
            flushReports();
        });
    }

    bool connectToInfra() override {
        LOG_INFO("[MOCK EXECUTION] Connected to Infra (Mock).");
        return true;
    }

    void disconnectFromInfra() override {
        LOG_INFO("[MOCK EXECUTION] Disconnected from Infra (Mock).");
    }

    std::string placeOrder(const OrderRequest& req) override {
        LOG_INFO("[MOCK EXECUTION] Placed {} Order: {} {:.4f} {} | Client ID: {}",
                 req.type == OrderType::MARKET_ON_CLOSE ? "MOC" : "MKT",
                 req.side == Side::BUY ? "BUY" : "SELL", req.quantity, req.symbol, req.client_order_id);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (req.type == OrderType::MARKET_ON_CLOSE) {
                pending_moc_.push_back({req, open_session_, "mock-" + std::to_string(++order_counter_)});
                pending_reports_.push_back(statusReport(pending_moc_.back().req, pending_moc_.back().order_id, OrderStatus::NEW));
            } else {
                auto it = latest_prices_.find(req.symbol);
                std::string oid = "mock-" + std::to_string(++order_counter_);
                if (it == latest_prices_.end() || it->second <= 0.0) {
                    LOG_ERROR("[MOCK EXECUTION] FAILED to fill {}. No price data available in cache!", req.symbol);
                    pending_reports_.push_back(statusReport(req, oid, OrderStatus::REJECTED));
                } else {
                    pending_reports_.push_back(fillLocked(req, oid, it->second, open_session_));
                }
            }
        }
        flushReports();
        return req.client_order_id;  // IExecutionEngine contract: return the request's id
    }

    bool cancelOrder(const std::string& orderId) override {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto it = pending_moc_.begin(); it != pending_moc_.end(); ++it) {
                if (it->req.client_order_id == orderId || it->order_id == orderId) {
                    pending_reports_.push_back(statusReport(it->req, it->order_id, OrderStatus::CANCELED));
                    pending_moc_.erase(it);
                    break;
                }
            }
        }
        flushReports();
        return true;
    }

    bool cancelAllOrders() override {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (const auto& o : pending_moc_) ids.push_back(o.req.client_order_id);
        }
        for (const auto& id : ids) cancelOrder(id);
        return true;
    }

    std::optional<double> getPortfolioValue() override { return 100000.0; }
    std::optional<double> getAvailableCash() override { return 100000.0; }

    std::vector<Position> getOpenPositions() override {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Position> out;
        for (const auto& [sym, qty] : positions_) {
            if (std::abs(qty) > 1e-9) out.push_back(Position{sym, qty, 0.0, 0.0, 0.0});
        }
        return out;
    }

    std::vector<OrderState> getOpenOrders() override { return {}; }

private:
    struct PendingMoc {
        OrderRequest req;
        int64_t session_day;  // fills at this session's close
        std::string order_id;
    };

    ExecutionReport statusReport(const OrderRequest& req, const std::string& oid, OrderStatus st) const {
        ExecutionReport r;
        r.order_id = oid;
        r.client_order_id = req.client_order_id;
        r.symbol = req.symbol;
        r.side = req.side;
        r.status = st;
        return r;
    }

    ExecutionReport fillLocked(const OrderRequest& req, const std::string& oid, double price, int64_t day) {
        ExecutionReport r = statusReport(req, oid, OrderStatus::FILLED);
        r.exec_id = oid + "-1";
        r.last_quantity = req.quantity;
        r.cumulative_quantity = req.quantity;
        r.last_price = price;
        r.avg_price = price;
        positions_[req.symbol] += (req.side == Side::BUY ? req.quantity : -req.quantity);
        // Machine-readable for scripts/parity_check.py: FILL day=<days since epoch> ...
        LOG_INFO("[MOCK EXECUTION] FILL day={} {} {} {:.4f} @ {:.4f}", day, req.side == Side::BUY ? "BUY" : "SELL",
                 req.symbol, req.quantity, price);
        return r;
    }

    void fillMocOrdersLocked(const std::string& symbol, int64_t bar_day, double close) {
        for (auto it = pending_moc_.begin(); it != pending_moc_.end();) {
            if (it->req.symbol == symbol && bar_day >= it->session_day) {
                pending_reports_.push_back(fillLocked(it->req, it->order_id, close, bar_day));
                it = pending_moc_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Reports are published outside the lock; the bus may deliver them inline.
    void flushReports() {
        std::vector<ExecutionReport> out;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            out.swap(pending_reports_);
        }
        for (const auto& r : out) event_bus_->publishExecution(r);
    }

    std::shared_ptr<IEventBus> event_bus_;
    std::mutex mtx_;
    std::unordered_map<std::string, double> latest_prices_;
    std::unordered_map<std::string, double> positions_;
    std::vector<PendingMoc> pending_moc_;
    std::vector<ExecutionReport> pending_reports_;
    int64_t open_session_ = 0;
    uint64_t order_counter_ = 0;
};

} // namespace bot
