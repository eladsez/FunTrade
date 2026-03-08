#pragma once

#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>
#include <nlohmann/json.hpp>

namespace bot {

// Forward declaration to hide Boost.Beast types from the header
struct WsClientState;

class AlpacaDataFeed : public IDataFeed {
public:
    // Inject the Event Bus and credentials.
    AlpacaDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret);
    ~AlpacaDataFeed() override;

    // IDataFeed Implementation
    bool connect() override;
    void subscribeToSymbol(const std::string& symbol) override;
    void disconnect() override;

    // Public callback intended to be called by the underlying WebSocket library
    // when a text message frame is received.
    void onWebSocketMessage(const std::string& payload);

protected:
    std::atomic<bool> isConnected_{false};
    std::atomic<bool> isRunning_{false};
    
    // Network abstractions that can be mocked in tests
    virtual void sendWebSocketMessage(const std::string& message);
    virtual void wsRunLoop();

private:
    std::shared_ptr<IEventBus> eventBus_;
    std::string apiKey_;
    std::string apiSecret_;
    
    std::mutex symbolsMutex_;
    std::vector<std::string> subscribedSymbols_;

    std::unique_ptr<WsClientState> wsState_;
    std::thread wsThread_;

    std::mutex connectMutex_;
    std::condition_variable connectCv_;

    // Internal parsing strategies for different stream types
    void parseTradeUpdate(const nlohmann::json& data);
    void parseQuoteUpdate(const nlohmann::json& data);
    void parseOrderBookUpdate(const nlohmann::json& data);
    
    // Asynchronous write loop
    void doWrite();
};

} // namespace bot