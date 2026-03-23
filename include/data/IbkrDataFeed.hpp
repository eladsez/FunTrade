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
    IbkrDataFeed(std::shared_ptr<IbkrInfra> infra);
    ~IbkrDataFeed() override = default;

    bool connectToInfra() override;
    void disconnectFromInfra() override;

    void subscribeToSymbol(const std::string& symbol) override;
    void unsubscribeFromSymbol(const std::string& symbol) override;
    std::vector<std::string> getActiveSubscriptions() const override;

    std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, const std::string& start, 
        const std::string& end, const std::string& timeframe) override;

    void setMarketTickCallback(std::function<void(const MarketEvent&)> callback) override;

private:
    // The new atomic callbacks
    void onTickByTickBidAsk(int reqId, time_t time, double bidPrice, double askPrice, Decimal bidSize, Decimal askSize, const TickAttribBidAsk& tickAttrib);
    void onTickByTickAllLast(int reqId, int tickType, time_t time, double price, Decimal size, const TickAttribLast& tickAttrib, const std::string& exchange, const std::string& specialConditions);

    std::string getSymbolFromId(int reqId);
    Contract createContract(const std::string& symbol);

    std::shared_ptr<IbkrInfra> infra_;
    
    std::function<void(const MarketEvent&)> tickCallback_;

    mutable std::mutex dataMutex_;

    // ID Management
    std::atomic<int> nextTickerId_{2000}; 

    // Because TickByTick requires separate IDs for Quotes and Trades
    struct StreamIds {
        int bidAskId;
        int allLastId;
    };
    
    std::unordered_map<std::string, StreamIds> symbolToIds_;
    std::unordered_map<int, std::string> idToSymbol_; // Maps any ID back to its symbol
};

} // namespace bot