#pragma once
#include <string>
#include <cstdint>
#include <variant>
#include <vector>
#include <atomic>
#include <chrono>

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

struct MidPointTick {
    std::string symbol;
    double mid_price = 0.0;
    uint64_t timestamp_ms = 0;
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
using MarketEvent = std::variant<TradeTick, QuoteTick, MidPointTick, OrderBookUpdate, BarUpdate>;

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
    std::string client_order_id; // Unique per order: OrderIdUtils::newClientOrderId(position_key)
    std::string symbol;
    Side side = Side::UNKNOWN;
    OrderType type = OrderType::MARKET;
    TimeInForce tif = TimeInForce::GTC; // Default to GTC to match your current logic
    double quantity = 0.0;
    double price = 0.0;       // For LIMIT and STOP_LIMIT
    double stop_price = 0.0;  // For STOP and STOP_LIMIT
    double trail_price = 0.0;  // For TRAILING_STOP (absolute distance)
    double trail_percent = 0.0;  // For TRAILING_STOP (percentage distance, e.g., 1.0 for 1%)
};

// Represents a notification from the exchange about an order's status change.
// A report is a FILL when last_quantity > 0; it then carries exec_id, which is unique per
// execution so a redelivered fill can be recognised. Status-only reports have last_quantity == 0.
// Engines must report client_order_id exactly as it was sent in the OrderRequest.
struct ExecutionReport {
    std::string order_id;        // ID assigned by the exchange
    std::string client_order_id; // ID assigned by the client
    std::string exec_id;         // Unique execution ID (fills only)
    std::string symbol;
    Side side = Side::UNKNOWN;
    OrderStatus status = OrderStatus::NEW;
    double last_quantity = 0.0;       // Quantity filled in this specific update
    double cumulative_quantity = 0.0; // Total quantity filled so far
    double last_price = 0.0;          // Price of the last fill
    double avg_price = 0.0;           // Average price of all fills for this order
    uint64_t timestamp_ms = 0;
};

inline bool isTerminal(OrderStatus s) {
    return s == OrderStatus::FILLED || s == OrderStatus::CANCELED ||
           s == OrderStatus::REJECTED || s == OrderStatus::EXPIRED;
}

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
    Side side = Side::UNKNOWN;
    OrderType type = OrderType::MARKET;
    OrderStatus status = OrderStatus::NEW;

    double price = 0.0;              // Requested limit/stop price
    double quantity = 0.0;           // Target total quantity
    double filled_quantity = 0.0;    // Cumulative quantity filled so far
    double avg_price = 0.0;          // Average execution price
};

namespace OrderIdUtils {

    // Client order IDs have the form "<position_key>#<unique suffix>". The position key
    // (e.g. "hban_mco_schw_HBAN") names the virtual position a fill belongs to; the suffix
    // makes every order unique, so a late report from an earlier cycle can never be
    // mistaken for the current one. Basket IDs never contain '#'.
    inline constexpr char kSeparator = '#';

    // Thread-safe; unique across restarts (epoch ms) and within a millisecond (counter).
    inline std::string newClientOrderId(const std::string& positionKey) {
        static std::atomic<uint32_t> counter{0};
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        return positionKey + kSeparator + std::to_string(ms) + "-" + std::to_string(++counter);
    }

    // "hban_mco_schw_HBAN#1792436188331-7" -> "hban_mco_schw_HBAN". IDs without a suffix
    // (e.g. orders placed outside the bot) are their own position key.
    inline std::string positionKeyOf(const std::string& clientOrderId) {
        auto pos = clientOrderId.find(kSeparator);
        return pos == std::string::npos ? clientOrderId : clientOrderId.substr(0, pos);
    }

} // namespace OrderIdUtils

}