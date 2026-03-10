#pragma once
#include <string>
#include <vector>
#include <core/DomainModels.hpp>

// Interface for connecting to market data sources (e.g., Binance, Alpaca, or a local CSV file).
class IDataFeed {
public:
    virtual ~IDataFeed() = default;

    // Connects to the exchange and starts the data stream.
    virtual bool connectToInfra() = 0;

    // Subscribes to real-time updates for a specific market symbol.
    virtual void subscribeToSymbol(const std::string& symbol) = 0;

    // Gracefully shuts down the connection.
    virtual void disconnectFromInfra() = 0;

    // Fetches historical OHLCV bars for a symbol
    virtual std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string& start, 
        const std::string& end, 
        const std::string& timeframe) = 0;
};