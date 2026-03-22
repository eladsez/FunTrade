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
    IbkrExecutionEngine(std::shared_ptr<IbkrInfra> infra, IEventBus* eventBus);
    ~IbkrExecutionEngine() override = default;

    // --- IExecutionEngine Implementation ---
    bool connectToInfra() override;
    void disconnectFromInfra() override;

    [[nodiscard]] std::string placeOrder(const OrderRequest& request) override;
    
    bool cancelOrder(const std::string& client_order_id) override;
    bool cancelAllOrders() override;

    // Synchronous Account/Portfolio Methods
    std::optional<double> getAvailableCash() override;
    std::optional<double> getPortfolioValue() override;
    std::vector<Position> getOpenPositions() override { return std::vector<Position>(); }
    std::vector<OrderState> getOpenOrders() override { return std::vector<OrderState>(); }

private:
    // Callbacks from IbkrInfra
    void onNextValidId(int orderId);
    void onOrderStatus(int orderId, const std::string& status, Decimal filled, Decimal remaining, 
                       double avgFillPrice, int permId, int parentId, double lastFillPrice, 
                       int clientId, const std::string& whyHeld, double mktCapPrice);
    void onExecDetails(int reqId, const Contract& contract, const Execution& execution);

    // Helpers
    Contract createContract(const std::string& symbol);
    std::string translateTimeInForce(TimeInForce tif);
    OrderStatus parseIbkrStatus(const std::string& status);

    std::shared_ptr<IbkrInfra> infra_;
    IEventBus* eventBus_;

    std::mutex idMutex_;
    std::atomic<int> nextValidId_{0};
    bool validIdReceived_{false};
    std::condition_variable idCv_; 

    std::unordered_map<std::string, int> clientRefToOrderId_;
    std::unordered_map<int, std::string> orderIdToClientRef_;

    std::atomic<double> cachedCash_{0.0};
    std::atomic<double> cachedPortfolioValue_{0.0};
};

} // namespace bot