#include <data/MockDataFeed.hpp>
#include <core/Logger.hpp>

MockDataFeed::MockDataFeed(std::shared_ptr<IEventBus> bus) : eventBus(bus) {}

bool MockDataFeed::connect() {
    LOG_INFO("[MockDataFeed] Connected to mock feed.");
    return true;
}

void MockDataFeed::subscribeToSymbol(const std::string& symbol) {
    LOG_INFO("[MockDataFeed] Subscribing to {} on feed.", symbol);
}

void MockDataFeed::disconnect() {
    LOG_INFO("[MockDataFeed] Disconnected.");
}

void MockDataFeed::simulateTick(const MarketTick& tick) {
    LOG_INFO("[MockDataFeed] Simulating incoming tick for {} @ {}", tick.symbol, tick.price);
    if (eventBus) {
        eventBus->publishTick(tick);
    }
}
