#include <strategy/MarketEventPrinter.hpp>
#include <core/Logger.hpp>
#include <cmath>
#include <algorithm>

namespace bot {

MarketEventPrinter::MarketEventPrinter(std::shared_ptr<IEventBus> bus, const std::string& symbol)
    : eventBus(bus), symbol_(symbol) {}

void MarketEventPrinter::start() {
    LOG_INFO("[MarketEventPrinter] Strategy started");

    eventBus->subscribeToMarketEvents(symbol_, [this](const MarketEvent& event) {
        this->onMarketEvent(event);
    });
}

std::string takerSideToString(Side side) {
        switch (side) {
            case Side::BUY: return "BUY";
            case Side::SELL: return "SELL";
            default: return "UNKNOWN";
        }
    }

std::string tradeConditionToString(TradeCondition cond) {
    switch (cond) {
        case TradeCondition::REGULAR: return "@";
        case TradeCondition::ISO: return "ISO(F)";
        case TradeCondition::ODD_LOT: return "ODD(I)";
        case TradeCondition::EXTENDED_HOURS: return "EXT(U)";
        case TradeCondition::LATE_PRINT: return "LATE(Z)";
        case TradeCondition::CASH_TRADE: return "CASH(C)";
        case TradeCondition::PRIOR_REFERENCE: return "PRIOR(P)";
        default: return "OTHER";
    }
}

std::string quoteConditionToString(QuoteCondition cond) {
    switch (cond) {
        case QuoteCondition::REGULAR: return "REGULAR";
        case QuoteCondition::NON_FIRM: return "NON_FIRM(R)";
        case QuoteCondition::CLOSING: return "CLOSING(C)";
        default: return "OTHER";
    }
}

void printMarketEvent(const MarketEvent& event) {
    std::visit([](auto&& tick) {
        using T = std::decay_t<decltype(tick)>;

        if constexpr (std::is_same_v<T, TradeTick>) {
            // Format the conditions array into a readable string
            std::string condStr = "[";
            for (size_t i = 0; i < tick.conditions.size(); ++i) {
                condStr += tradeConditionToString(tick.conditions[i]);
                if (i < tick.conditions.size() - 1) condStr += ", ";
            }
            condStr += "]";

            LOG_INFO("[MarketEvent] TRADE | {}: Price: {:.2f}, Vol: {:.4f}, Taker: {}, Cond: {}, TS: {}", 
                     tick.symbol, 
                     tick.price, 
                     tick.volume, 
                     takerSideToString(tick.taker_side),
                     condStr,
                     tick.timestamp_ms);
        } 
        else if constexpr (std::is_same_v<T, QuoteTick>) {
            // Format the quote conditions array
            std::string condStr = "[";
            for (size_t i = 0; i < tick.conditions.size(); ++i) {
                condStr += quoteConditionToString(tick.conditions[i]);
                if (i < tick.conditions.size() - 1) condStr += ", ";
            }
            condStr += "]";

            LOG_INFO("[MarketEvent] QUOTE | {}: Bid {:.4f}@{:.2f} | Ask {:.4f}@{:.2f}, Cond: {}, TS: {}", 
                     tick.symbol, 
                     tick.bid_size, tick.bid_price, 
                     tick.ask_size, tick.ask_price, 
                     condStr,
                     tick.timestamp_ms);
        } 
        else if constexpr (std::is_same_v<T, OrderBookUpdate>) {
            // Safely grab the top of the book for a quick preview if it exists
            double bestBid = tick.bids.empty() ? 0.0 : tick.bids.front().price;
            double bestAsk = tick.asks.empty() ? 0.0 : tick.asks.front().price;
            
            LOG_INFO("[MarketEvent] ORDERBOOK | {}: Levels (Bid/Ask): {}/{} | Top Bid: {:.2f} | Top Ask: {:.2f} | Snapshot: {} | TS: {}", 
                     tick.symbol, 
                     tick.bids.size(), 
                     tick.asks.size(), 
                     bestBid, 
                     bestAsk, 
                     tick.is_snapshot ? "YES" : "NO",
                     tick.timestamp_ms);
        }
    }, event);
}

void MarketEventPrinter::onMarketEvent(const MarketEvent& event) {
    printMarketEvent(event);
}

void MarketEventPrinter::stop() {
    LOG_INFO("[MarketEventPrinter] Strategy stopped.");
}

} // namespace bot