#pragma once

#include <execution/IExecutionEngine.hpp>
#include <string>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

#include <core/IEventBus.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <atomic>

namespace bot {

// Forward declaration to hide Boost.Beast types from the header
struct AlpacaWsState;

class AlpacaExecutionEngine : public IExecutionEngine {
public:
    AlpacaExecutionEngine(IEventBus& eventBus, std::string apiKey, std::string apiSecret, std::string baseUrl);
    ~AlpacaExecutionEngine() override;

    // IExecutionEngine Implementation
    bool authenticate() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;

    // Extended Domain Specific API
    std::optional<double> getPortfolioValue() override;
    nlohmann::json getOpenPositions() override;
    nlohmann::json getOpenOrders() override;

protected:
    // Abstracted HTTP request layer
    virtual std::string sendHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "");
    
    // Asynchronous version for non-blocking placeOrder
    void sendHttpRequestAsync(const std::string& method, const std::string& endpoint, const std::string& payload, 
                             std::function<void(const std::string&)> callback);

private:
    IEventBus& eventBus_;
    std::string apiKey_;
    std::string apiSecret_;
    std::string baseUrl_;

    // Asio components for async operations
    boost::asio::io_context ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_;
    std::thread ioThread_;

    // WebSocket state for trade updates
    std::unique_ptr<AlpacaWsState> wsState_;
    std::thread wsThread_;
    std::atomic<bool> isWsRunning_{false};

    void runWsLoop();
    void onWsMessage(const std::string& payload);
};

} // namespace bot
