#include <execution/IbkrOrderTracker.hpp>
#include <core/Logger.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace bot {

namespace {

constexpr double kEps = 1e-9;

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

OrderStatus IbkrOrderTracker::parseStatus(const std::string& s) {
    if (s == "Filled") return OrderStatus::FILLED;
    if (s == "Cancelled" || s == "ApiCancelled") return OrderStatus::CANCELED;
    if (s == "Inactive") return OrderStatus::REJECTED;
    // PendingSubmit, PreSubmitted, Submitted, ApiPending, PendingCancel: still working.
    // PendingCancel in particular can still fill, so it must NOT be terminal.
    return OrderStatus::NEW;
}

bool IbkrOrderTracker::isTerminalErrorCode(int code) {
    switch (code) {
        case 103:  // Duplicate order id
        case 110:  // Price does not conform to the minimum price variation
        case 200:  // No security definition found
        case 201:  // Order rejected
        case 202:  // Order cancelled
        case 203:  // Security not available/allowed for this account
            return true;
        default:
            return false;
    }
}

void IbkrOrderTracker::onPlaced(long orderId, const OrderRequest& request) {
    std::lock_guard<std::mutex> lock(mtx_);
    Tracked t;
    t.client_order_id = request.client_order_id;
    t.symbol = request.symbol;
    t.side = request.side;
    t.quantity = request.quantity;
    orders_[orderId] = t;
    idByClient_[request.client_order_id] = orderId;
}

void IbkrOrderTracker::onOpenOrder(long orderId, const std::string& orderRef, const std::string& symbol,
                                   Side side, double quantity) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (orders_.count(orderId)) return;  // already known (placed by us this session)
    Tracked t;
    t.client_order_id = orderRef;
    t.symbol = symbol;
    t.side = side;
    t.quantity = quantity;
    orders_[orderId] = t;
    if (!orderRef.empty()) idByClient_[orderRef] = orderId;
}

std::optional<long> IbkrOrderTracker::orderIdFor(const std::string& clientOrderId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = idByClient_.find(clientOrderId);
    if (it == idByClient_.end()) return std::nullopt;
    return it->second;
}

bool IbkrOrderTracker::isTracked(long orderId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return orders_.count(orderId) > 0;
}

IbkrOrderTracker::Tracked* IbkrOrderTracker::find(long orderId) {
    auto it = orders_.find(orderId);
    return it == orders_.end() ? nullptr : &it->second;
}

ExecutionReport IbkrOrderTracker::baseReport(long orderId, const Tracked& t) const {
    ExecutionReport r;
    r.order_id = std::to_string(orderId);
    r.client_order_id = t.client_order_id;
    r.symbol = t.symbol;
    r.side = t.side;
    r.cumulative_quantity = t.exec_filled;
    r.avg_price = t.avg_price;
    r.timestamp_ms = nowMs();
    return r;
}

std::optional<ExecutionReport> IbkrOrderTracker::onOrderStatus(long orderId, const std::string& status,
                                                               double filled, double avgFillPrice) {
    std::lock_guard<std::mutex> lock(mtx_);
    Tracked* t = find(orderId);
    if (!t || isTerminal(t->reported)) return std::nullopt;

    OrderStatus st = parseStatus(status);
    if (st == OrderStatus::NEW && filled > kEps) st = OrderStatus::PARTIALLY_FILLED;
    if (avgFillPrice > 0.0) t->avg_price = avgFillPrice;

    if (isTerminal(st) && t->exec_filled + kEps < filled) {
        // Don't announce the end of the order before its executions arrive; onExecution
        // releases the held status once they have.
        t->held_terminal = st;
        t->held_filled = filled;
        return std::nullopt;
    }

    // Report the cumulative we can vouch for (executions seen), never more.
    double cum = std::min(filled, t->exec_filled);
    if (st == t->reported && std::abs(cum - t->reported_cum) < kEps) return std::nullopt;  // repeat

    t->reported = st;
    t->reported_cum = cum;
    ExecutionReport r = baseReport(orderId, *t);
    r.status = st;
    r.cumulative_quantity = cum;
    return r;
}

std::optional<ExecutionReport> IbkrOrderTracker::onExecution(long orderId, const std::string& orderRef,
                                                             const std::string& execId, const std::string& symbol,
                                                             const std::string& ibSide, double shares,
                                                             double price, double avgPrice) {
    std::lock_guard<std::mutex> lock(mtx_);
    Tracked* t = find(orderId);
    if (!t && !orderRef.empty()) {
        auto it = idByClient_.find(orderRef);
        if (it != idByClient_.end()) { orderId = it->second; t = find(orderId); }
    }
    if (!t) {
        LOG_WARN("[IbkrOrderTracker] Execution {} for untracked order {} (ref '{}', {} {} {}) ignored.",
                 execId, orderId, orderRef, ibSide, shares, symbol);
        return std::nullopt;
    }
    if (shares <= 0.0) return std::nullopt;
    if (!execId.empty() && !seenExecIds_.insert(execId).second) return std::nullopt;  // redelivered

    t->exec_filled += shares;
    if (avgPrice > 0.0) t->avg_price = avgPrice;

    OrderStatus st;
    if (isTerminal(t->reported)) {
        st = t->reported;  // late execution after the order ended: still a real fill
    } else if (t->held_terminal && t->exec_filled + kEps >= t->held_filled) {
        st = *t->held_terminal;
        t->held_terminal.reset();
    } else if (t->exec_filled + kEps >= t->quantity) {
        st = OrderStatus::FILLED;
    } else {
        st = OrderStatus::PARTIALLY_FILLED;
    }

    t->reported = st;
    t->reported_cum = t->exec_filled;
    ExecutionReport r = baseReport(orderId, *t);
    r.exec_id = execId;
    r.status = st;
    if (r.symbol.empty()) r.symbol = symbol;
    if (r.side == Side::UNKNOWN) r.side = (ibSide == "BOT") ? Side::BUY : Side::SELL;
    r.last_quantity = shares;
    r.last_price = price;
    return r;
}

std::optional<ExecutionReport> IbkrOrderTracker::onError(long id, int errorCode) {
    if (!isTerminalErrorCode(errorCode)) return std::nullopt;
    std::lock_guard<std::mutex> lock(mtx_);
    Tracked* t = find(id);
    if (!t || isTerminal(t->reported)) return std::nullopt;

    OrderStatus st = (errorCode == 202) ? OrderStatus::CANCELED : OrderStatus::REJECTED;
    t->reported = st;
    t->reported_cum = t->exec_filled;
    t->held_terminal.reset();
    ExecutionReport r = baseReport(id, *t);
    r.status = st;
    return r;
}

} // namespace bot
