#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <future>

namespace bot {

// Forward declare the internal Boost.Asio state to keep the header clean
struct InfraState;

enum class WsType { DATA, EXECUTION };

class AlpacaInfra {
public:
    AlpacaInfra(std::string apiKey, std::string apiSecret);
    ~AlpacaInfra();

    // --- WebSocket API (For Real-Time Data) ---
    bool connectWs(const std::string& host, const std::string& path, WsType type);
    void disconnectWs();
    void sendWsMessage(const std::string& message);
    
    // Register a callback so the DataFeed gets notified on new messages
    void setWsMessageCallback(std::function<void(const std::string&)> callback);

    // --- REST API (For History & Execution) ---
    // Performs an HTTPS GET or POST to Alpaca's REST endpoints
    std::string sendRestRequest(const std::string& method, const std::string& host, const std::string& target, const std::string& payload = "");

private:

    WsType currentWsType_;
    bool isAuthenticating_{false};
    std::shared_ptr<std::promise<bool>> authPromise_;

    std::string apiKey_;
    std::string apiSecret_;

    std::unique_ptr<InfraState> state_;
    std::thread ioThread_;
    
    std::function<void(const std::string&)> wsCallback_;
    std::atomic<bool> isWsConnected_{false};

    void runIoContext();
    void doWsRead();
    void doWsWrite();
    void handleAuthMessage(const std::string& payload);
};

} // namespace bot