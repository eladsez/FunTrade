#pragma once

#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <infra/AlpacaInfra.hpp>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace bot {

class AlpacaRtDataFeed : public IDataFeed {
public:
    // We now inject the shared AlpacaInfra instance
    AlpacaRtDataFeed(std::string apiKey, std::string apiSecret);
    ~AlpacaRtDataFeed() override;

    bool connectToInfra() override;
    void disconnectFromInfra() override;
    void subscribeToSymbol(const std::string& symbol) override;
    void unsubscribeFromSymbol(const std::string& symbol) override;
    std::vector<std::string> getActiveSubscriptions() const override;
    std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string& start, 
        const std::string& end, 
        const std::string& timeframe) override;
    void setMarketTickCallback(std::function<void(const MarketEvent&)> callback) override;

private:
    std::shared_ptr<IEventBus> eventBus_;
    std::shared_ptr<AlpacaInfra> infra_;
    std::string apiKey_;
    std::string apiSecret_;
    std::function<void(const MarketEvent&)> tickCallback_;
    std::unordered_set<std::string> activeSubscriptions_;

    // Debouncer members
    std::unordered_set<std::string> pendingSubscriptions_;
    std::unordered_set<std::string> pendingUnsubscriptions_;
    mutable std::mutex batchMutex_;
    std::condition_variable batchCv_;
    std::thread batchThread_;
    std::atomic<bool> stopBatcher_;

    void batcherLoop(); // The background thread execution loop

    // The callback triggered by AlpacaInfra
    void onWebSocketMessage(const std::string& payload);

    void parseTradeUpdate(const nlohmann::json& data);
    void parseQuoteUpdate(const nlohmann::json& data);
    void parseOrderBookUpdate(const nlohmann::json& data);

    Side stringToSide(const std::string& side);
    TradeCondition stringToTradeCondition(const std::string& cond);
    QuoteCondition stringToQuoteCondition(const std::string& cond);
};

} // namespace bot