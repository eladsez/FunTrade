#include <data/MockDataFeed.hpp>
#include <iostream>

MockDataFeed::MockDataFeed(std::shared_ptr<IEventBus> bus) : eventBus(bus) {}

bool MockDataFeed::connect() {
    std::cout << "[MockDataFeed] Connected to mock feed.\n";
    return true;
}

void MockDataFeed::subscribeToSymbol(const std::string& symbol) {
    std::cout << "[MockDataFeed] Subscribing to " << symbol << " on feed.\n";
}

void MockDataFeed::disconnect() {
    std::cout << "[MockDataFeed] Disconnected.\n";
}

void MockDataFeed::simulateTick(const MarketTick& tick) {
    std::cout << "[MockDataFeed] Simulating incoming tick for " << tick.symbol << " @ " << tick.price << "\n";
    if (eventBus) {
        eventBus->publishTick(tick);
    }
}
