#pragma once
#include <execution/IExecutionEngine.hpp>
#include <core/IEventBus.hpp>
#include <functional>
#include <vector>
#include <string>

namespace bot::test {

// Scriptable IExecutionEngine: records every order and lets the test emit exactly the
// execution reports it wants (fills, partials, cancels, rejects, duplicates, out-of-order).
class FakeExecutionEngine : public IExecutionEngine {
public:
    bool connectToInfra() override { return true; }
    void disconnectFromInfra() override {}

    std::string placeOrder(const OrderRequest& request) override {
        placed.push_back(request);
        if (failPlacements) return "";  // never reached the exchange
        if (onPlace) onPlace(request);
        return request.client_order_id;
    }

    bool cancelOrder(const std::string& orderId) override {
        canceled.push_back(orderId);
        if (onCancel) onCancel(orderId);
        return true;
    }

    // The placed order a cancel id refers to (client id, or the fake exchange id "X-<cid>").
    const OrderRequest* findPlaced(const std::string& id) const {
        for (const auto& r : placed)
            if (r.client_order_id == id || "X-" + r.client_order_id == id) return &r;
        return nullptr;
    }
    bool cancelAllOrders() override { cancelAllCalls++; return true; }

    std::optional<double> getPortfolioValue() override { return portfolioValue; }
    std::optional<double> getAvailableCash() override { return availableCash; }
    std::vector<Position> getOpenPositions() override { return openPositions; }
    std::vector<OrderState> getOpenOrders() override { return openOrders; }

    // Emits a report through the callback the event bus registered (if any), else via the bus.
    void emit(const ExecutionReport& report) {
        if (reportCb_) reportCb_(report);
        else if (bus) bus->publishExecution(report);
    }

    // Builds a fully initialised report for a previously placed order. A report with
    // lastQty > 0 is a fill and gets a fresh exec_id unless one is given.
    static ExecutionReport report(const OrderRequest& req, OrderStatus status, double lastQty,
                                  double cumQty, double price, const std::string& exchId = "",
                                  const std::string& execId = "") {
        static int execCounter = 0;
        ExecutionReport r{};
        if (lastQty > 0) r.exec_id = execId.empty() ? "E" + std::to_string(++execCounter) : execId;
        r.order_id = exchId.empty() ? "X-" + req.client_order_id : exchId;
        r.client_order_id = req.client_order_id;
        r.symbol = req.symbol;
        r.side = req.side;
        r.status = status;
        r.last_quantity = lastQty;
        r.cumulative_quantity = cumQty;
        r.last_price = price;
        r.avg_price = price;
        r.timestamp_ms = 0;
        return r;
    }

    // Fills every recorded order (from index `from`) completely at the given per-symbol prices.
    void fillAll(const std::function<double(const std::string&)>& priceOf, size_t from = 0) {
        for (size_t i = from; i < placed.size(); ++i) {
            const auto& req = placed[i];
            emit(report(req, OrderStatus::FILLED, req.quantity, req.quantity, priceOf(req.symbol)));
        }
    }

    IEventBus* bus = nullptr;
    std::function<void(const OrderRequest&)> onPlace;
    std::function<void(const std::string&)> onCancel;
    std::vector<OrderRequest> placed;
    std::vector<std::string> canceled;
    int cancelAllCalls = 0;
    bool failPlacements = false;

    std::optional<double> portfolioValue = 100000.0;
    std::optional<double> availableCash = 100000.0;
    std::vector<Position> openPositions;
    std::vector<OrderState> openOrders;
};

} // namespace bot::test
