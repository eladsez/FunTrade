#pragma once
#include <string>
#include <vector>
#include <core/DomainModels.hpp>
#include <functional>

namespace bot {

// Interface for connecting to market data sources (e.g., Binance, Alpaca, or a local CSV file).
class IDataFeed {
public:
    virtual ~IDataFeed() = default;

    // Connects to the exchange and starts the data stream and gracefully shuts down the connection
    virtual bool connectToInfra() = 0;
    virtual void disconnectFromInfra() = 0;

    // Subscribes/Unsubscribes to real-time updates for a specific market symbol.
    virtual void subscribeToSymbol(const std::string& symbol) = 0;
    virtual void unsubscribeFromSymbol(const std::string& symbol) = 0;
    virtual std::vector<std::string> getActiveSubscriptions() const = 0;

    // Fetches historical OHLCV bars for a symbol
    virtual std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string& start, 
        const std::string& end, 
        const std::string& timeframe) = 0;

    virtual void setMarketTickCallback(std::function<void(const MarketEvent&)> callback) = 0;
};

};