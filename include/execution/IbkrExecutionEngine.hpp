#pragma once

#include <execution/IExecutionEngine.hpp>
#include <infra/IbkrInfra.hpp>
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
    
    bool cancelOrder(const std::string& order_id);
    bool cancelAllOrders() override;

    // Synchronous Account/Portfolio Methods
    std::optional<double> getAvailableCash() override;
    std::optional<double> getPortfolioValue() override;
    std::vector<Position> getOpenPositions() override;
    std::vector<OrderState> getOpenOrders() override;

private:
    // Callbacks from IbkrInfra
    void onNextValidId(int orderId);
    void onOrderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining, 
                       double avgFillPrice, int permId, int parentId, double lastFillPrice, 
                       int clientId, const std::string& whyHeld, double mktCapPrice);
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
    OrderStatus parseIbkrStatus(const std::string& status);

    std::shared_ptr<IbkrInfra> infra_;

    std::mutex idMutex_;
    std::atomic<int> nextValidId_{0};
    bool validIdReceived_{false};
    std::condition_variable idCv_; 

    std::unordered_map<std::string, int> clientRefToOrderId_;
    std::unordered_map<int, std::string> orderIdToClientRef_;

    // Account State
    std::atomic<double> cachedCash_{0.0};
    std::atomic<bool> cashHasValue_{false};
    
    std::atomic<double> cachedPortfolioValue_{0.0};
    std::atomic<bool> portHasValue_{false};

    // Initialization Sync Barrier
    std::mutex syncMutex_;
    std::condition_variable syncCv_;
    bool isAccountSynced_{false};
    bool isPositionsSynced_{false};
    bool isOrdersSynced_{false};

    // Portfolio State
    std::mutex positionMutex_;
    std::unordered_map<std::string, Position> openPositions_; // Map by symbol

    std::mutex orderMutex_;
    std::unordered_map<int, OrderState> openOrders_; // Map by integer orderId
};

} // namespace bot