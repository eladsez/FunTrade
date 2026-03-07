#pragma once
#include <string>
#include <cstdint>

enum class Side {
    BUY,
    SELL
};

enum class OrderType {
    MARKET,
    LIMIT
};

// Represents a price update or a trade execution in the market.
struct MarketTick {
    std::string symbol;
    double price;         // Last trade price (or mid-price)
    double volume;        // Last trade volume
    uint64_t timestamp_ms;
    
    // Level 1 Order Book Data (Optional/0.0 if not provided by the feed)
    double bid_price = 0.0;
    double bid_size = 0.0;
    double ask_price = 0.0;
    double ask_size = 0.0;
};

// Represents an order we want to send to the exchange.
struct OrderRequest {
    std::string symbol;
    Side side;
    OrderType type;
    double quantity;
    double price; // Relevant only for LIMIT orders
};