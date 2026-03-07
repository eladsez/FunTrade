#pragma once

#include <execution/IExecutionEngine.hpp>
#include <string>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

namespace bot {

class AlpacaExecutionEngine : public IExecutionEngine {
public:
    AlpacaExecutionEngine(std::string apiKey, std::string apiSecret, std::string baseUrl);
    ~AlpacaExecutionEngine() override = default;

    // IExecutionEngine Implementation
    bool authenticate() override;
    std::string placeOrder(const OrderRequest& request) override;
    bool cancelOrder(const std::string& orderId) override;

    // Extended Domain Specific API
    std::optional<double> getPortfolioValue();
    nlohmann::json getOpenPositions();

private:
    std::string apiKey_;
    std::string apiSecret_;
    std::string baseUrl_;

    // Prevent concurrent modifications or request bursts via the same engine instance
    std::mutex requestMutex_;

    // Abstracted HTTP request layer (to be mapped to libcurl / Boost.Beast later)
    std::string sendHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "");
};

} // namespace bot
