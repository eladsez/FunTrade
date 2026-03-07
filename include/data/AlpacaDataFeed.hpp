#pragma once

#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

namespace bot {

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

private:
    std::shared_ptr<IEventBus> eventBus_;
    std::string apiKey_;
    std::string apiSecret_;
    
    std::atomic<bool> isConnected_{false};
    
    std::mutex symbolsMutex_;
    std::vector<std::string> subscribedSymbols_;

    // Internal parsing strategies for different stream types
    void parseTradeUpdate(const nlohmann::json& data);
    void parseQuoteUpdate(const nlohmann::json& data);
    void parseOrderBookUpdate(const nlohmann::json& data);

    // Mock network abstraction - this would be replaced by actual WS client calls
    void sendWebSocketMessage(const std::string& message);
};

} // namespace bot
