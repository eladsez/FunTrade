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
        if (onPlace) onPlace(request);
        return request.client_order_id;
    }

    bool cancelOrder(const std::string& orderId) override { canceled.push_back(orderId); return true; }
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

    // Builds a fully initialised report for a previously placed order.
    static ExecutionReport report(const OrderRequest& req, OrderStatus status, double lastQty,
                                  double cumQty, double price, const std::string& exchId = "") {
        ExecutionReport r{};
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
    std::vector<OrderRequest> placed;
    std::vector<std::string> canceled;
    int cancelAllCalls = 0;

    std::optional<double> portfolioValue = 100000.0;
    std::optional<double> availableCash = 100000.0;
    std::vector<Position> openPositions;
    std::vector<OrderState> openOrders;
};

} // namespace bot::test
