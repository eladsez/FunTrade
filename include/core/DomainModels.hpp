#pragma once
#include <string>
#include <cstdint>
#include <variant>
#include <vector>

namespace bot {

enum class Side {
    BUY,
    SELL,
    UNKNOWN // For stock trades where taker side isn't explicitly provided
};

enum class TradeCondition {
    REGULAR,         // '@' or ' '
    ISO,             // 'F' - Intermarket Sweep Order
    ODD_LOT,         // 'I'
    EXTENDED_HOURS,  // 'U'
    LATE_PRINT,      // 'Z'
    CASH_TRADE,      // 'C'
    PRIOR_REFERENCE, // 'P'
    OTHER            // Fallback for conditions we don't explicitly filter
};

enum class QuoteCondition {
    REGULAR,
    NON_FIRM,        // 'R'
    CLOSING,         // 'C'
    OTHER
};

struct PriceLevel {
    double price;
    double size;
};

struct TradeTick {
    std::string symbol;
    double price = 0.0;
    double volume = 0.0;
    uint64_t timestamp_ms = 0;
    Side taker_side = Side::UNKNOWN;
    std::vector<TradeCondition> conditions;
};

struct QuoteTick {
    std::string symbol;
    double bid_price = 0.0;
    double bid_size = 0.0;
    double ask_price = 0.0;
    double ask_size = 0.0;
    uint64_t timestamp_ms = 0;
    std::vector<QuoteCondition> conditions;
};

struct OrderBookUpdate {
    std::string symbol;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    uint64_t timestamp_ms = 0;
    bool is_snapshot = false; // Optional: Useful if Alpaca sends a full reset
};

struct BarUpdate {
    std::string symbol;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint64_t timestamp_ms = 0;
};

// The unified MarketEvent variant
using MarketEvent = std::variant<TradeTick, QuoteTick, OrderBookUpdate, BarUpdate>;

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

struct HistoricalBar {
    std::string symbol;
    uint64_t timestamp_ms = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint64_t trade_count = 0;
    double vwap = 0.0;
};


struct Position {
    std::string symbol;
    double quantity = 0.0;
    double avg_price = 0.0;
    double current_price = 0.0;
    double realized_pnl = 0.0;

    double unrealized_pnl() const {
        if (quantity == 0.0) return 0.0;
        return quantity * (current_price - avg_price);
    }
};

struct OrderState {
    std::string order_id;
    std::string client_order_id;
    std::string symbol;
    Side side;
    OrderType type;
    double quantity;
    double price;
    OrderStatus status;
};

namespace OrderIdUtils {

    // Safely extracts the original base prefix from a generated execution ID
    // E.g., "given_client_id_1710543210000_1" -> "given_client_id"
    inline std::string extractBaseClientId(const std::string& fullId) {
        if (fullId.empty()) return "";
        
        auto last_underscore = fullId.find_last_of('_');
        if (last_underscore == std::string::npos || last_underscore == 0) return fullId;
        
        auto second_last_underscore = fullId.find_last_of('_', last_underscore - 1);
        if (second_last_underscore == std::string::npos) return fullId;
        
        return fullId.substr(0, second_last_underscore);
    }

    // Checks if a full broker ID belongs to a specific base ID
    inline bool isMatchingId(const std::string& baseId, const std::string& fullId) {
        // The safest way is to extract and compare exactly
        return extractBaseClientId(fullId) == baseId;
    }

} // namespace OrderIdUtils

}