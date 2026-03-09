#pragma once

#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <infra/AlpacaInfra.hpp>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace bot {

class AlpacaRtDataFeed : public IDataFeed {
public:
    // We now inject the shared AlpacaInfra instance
    AlpacaRtDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret);
    ~AlpacaRtDataFeed() override;

    bool connectToInfra() override;
    void disconnectFromInfra() override;
    void subscribeToSymbol(const std::string& symbol) override;

private:
    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<AlpacaInfra> infra_;
    std::string apiKey_;
    std::string apiSecret_;

    std::mutex symbolsMutex_;
    std::vector<std::string> subscribedSymbols_;

    // The callback triggered by AlpacaInfra
    void onWebSocketMessage(const std::string& payload);

    void parseTradeUpdate(const nlohmann::json& data);
    void parseQuoteUpdate(const nlohmann::json& data);
    void parseOrderBookUpdate(const nlohmann::json& data);
};

} // namespace bot