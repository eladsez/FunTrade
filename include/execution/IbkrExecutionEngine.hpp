#pragma once

#include <execution/IExecutionEngine.hpp>
#include <infra/IbkrInfra.hpp>
#include <execution/IbkrOrderTracker.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace bot {

class IbkrExecutionEngine : public IExecutionEngine {
public:
    IbkrExecutionEngine(std::shared_ptr<IbkrInfra> infra);
    ~IbkrExecutionEngine() override = default;

    // --- IExecutionEngine Implementation ---
    bool connectToInfra() override;
    void disconnectFromInfra() override;

    [[nodiscard]] std::string placeOrder(const OrderRequest& request) override;
    
    // Accepts the IBKR order id or our client order id.
    bool cancelOrder(const std::string& order_id) override;
    bool cancelAllOrders() override;

    // Synchronous Account/Portfolio Methods
    std::optional<double> getAvailableCash() override;
    std::optional<double> getPortfolioValue() override;
    std::optional<std::string> getBaseCurrency();
    void setUsdRateFallback(double baseUnitsPerUsd);
    std::vector<Position> getOpenPositions() override;
    std::vector<OrderState> getOpenOrders() override;

private:
    // Callbacks from IbkrInfra
    void onNextValidId(int orderId);
    void onOrderStatus(int orderId, const std::string& status, Decimal filled, double avgFillPrice);
    void onExecDetails(int reqId, const Contract& contract, const Execution& execution);
    void onUpdateAccountValue(const std::string& key, const std::string& val, const std::string& currency, const std::string& accountName);
    void onPosition(const std::string& account, const Contract& contract, Decimal pos, double avgCost);
    void onOpenOrder(OrderId orderId, const Contract& contract, const ::Order& order, const ::OrderState& ibkrOrderState);
    void onError(int reqId, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson);
    void onAccountDownloadEnd(const std::string& accountName);
    void onPositionEnd();
    void onOpenOrderEnd();
    void onReconnect();

    // Helpers
    Contract createContract(const std::string& symbol);
    std::string translateTimeInForce(TimeInForce tif);

    std::shared_ptr<IbkrInfra> infra_;

    std::mutex idMutex_;
    std::atomic<int> nextValidId_{0};
    bool validIdReceived_{false};
    std::condition_variable idCv_; 

    static constexpr std::chrono::seconds kOrderIdTimeout{5};

    IbkrOrderTracker tracker_;

    // Account state. IBKR reports AvailableFunds / NetLiquidation in the account's BASE
    // currency (ILS on this account) and an "ExchangeRate" per currency (1 unit = rate base).
    // We trade USD stocks, so values are converted: usd = base / ExchangeRate[USD].
    std::mutex accountMutex_;
    std::optional<double> baseAvailableFunds_;
    std::optional<double> baseNetLiquidation_;
    std::optional<double> usdRate_;
    // TWS sends ExchangeRate[USD] only while the account holds USD; until then main sets
    // this from IBKR's FX quotes. A rate from TWS takes precedence.
    std::optional<double> usdRateFallback_;
    std::optional<std::string> baseCurrency_;
    std::optional<double> toUsd(const std::optional<double>& baseValue);

    // Initialization Sync Barrier
    std::mutex syncMutex_;
    std::condition_variable syncCv_;
    bool isAccountSynced_{false};
    bool isPositionsSynced_{false};
    bool isOrdersSynced_{false};
    bool isExecutionsSynced_{false};
    static constexpr int kExecutionsReqId = 7001;

    // Portfolio State
    std::mutex positionMutex_;
    std::unordered_map<std::string, Position> openPositions_; // Map by symbol

    void emit(const std::optional<ExecutionReport>& report);

    std::mutex orderMutex_;
    std::unordered_map<int, OrderState> openOrders_; // Map by integer orderId (from openOrder)
};

} // namespace bot