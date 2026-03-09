#pragma once
#include <string>

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
};