#pragma once
#include <string>
#include <cstdint>

enum class Side {
    BUY,
    SELL
};

enum class OrderType {
    MARKET,
    LIMIT,
    STOP,
    STOP_LIMIT,
    TRAILING_STOP
};

enum class OrderStatus {
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED,
    EXPIRED
};

enum class TickType {
    TRADE,
    QUOTE,
    ORDERBOOK
};

struct MarketTick {
    std::string symbol;
    TickType type;
    
    // Trade Data
    double price = 0.0;       // Last trade price
    double volume = 0.0;      // Last trade volume
    
    // Quote / Order Book Data
    double bid_price = 0.0;
    double bid_size = 0.0;
    double ask_price = 0.0;
    double ask_size = 0.0;

    uint64_t timestamp_ms = 0;
};

// Represents time-frame for an order.
enum class TimeInForce {
    GTC, // Good 'Til Canceled
    DAY, // Day Order
    IOC, // Immediate Or Cancel
    FOK  // Fill Or Kill
};

// Represents an order we want to send to the exchange.
struct OrderRequest {
    std::string client_order_id; 
    std::string symbol;
    Side side;
    OrderType type;
    TimeInForce tif = TimeInForce::GTC; // Default to GTC to match your current logic
    double quantity;
    double price = 0.0;       // For LIMIT and STOP_LIMIT
    double stop_price = 0.0;  // For STOP and STOP_LIMIT
    double trail_price = 0.0;  // For TRAILING_STOP (absolute distance)
    double trail_percent = 0.0;  // For TRAILING_STOP (percentage distance, e.g., 1.0 for 1%)
};

// Represents a notification from the exchange about an order's status change.
struct ExecutionReport {
    std::string order_id;        // ID assigned by the exchange
    std::string client_order_id; // ID assigned by the client
    std::string symbol;
    Side side;
    OrderStatus status;
    double last_quantity; // Quantity filled in this specific update
    double cumulative_quantity; // Total quantity filled so far
    double last_price;    // Price of the last fill
    double avg_price;     // Average price of all fills for this order
    uint64_t timestamp_ms;
};