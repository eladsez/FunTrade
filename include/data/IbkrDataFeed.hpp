#pragma once

#include <data/IDataFeed.hpp>
#include <infra/IbkrInfra.hpp>
#include <core/IEventBus.hpp>
#include <core/DomainModels.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

namespace bot {

class IbkrDataFeed : public IDataFeed {
public:
    IbkrDataFeed(std::shared_ptr<IbkrInfra> infra, IEventBus* eventBus);
    ~IbkrDataFeed() override = default;

    // --- IDataFeed Implementation ---
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
    void onTickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib);
    void onTickSize(TickerId tickerId, TickType field, Decimal size);

    int getOrCreateTickerId(const std::string& symbol);
    std::string getSymbolFromId(int tickerId);
    Contract createContract(const std::string& symbol);

    std::shared_ptr<IbkrInfra> infra_;
    IEventBus* eventBus_;
    std::function<void(const MarketEvent&)> tickCallback_;

    // Mutable so we can lock it inside const methods like getActiveSubscriptions
    mutable std::mutex dataMutex_;

    std::atomic<int> nextTickerId_{1000}; 
    std::unordered_map<std::string, int> symbolToId_;
    std::unordered_map<int, std::string> idToSymbol_;

    std::unordered_map<int, QuoteTick> quoteCache_;
    std::unordered_map<int, TradeTick> tradeCache_;
};

} // namespace bot