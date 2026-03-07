#pragma once
#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <memory>

class MockDataFeed : public IDataFeed {
public:
    explicit MockDataFeed(std::shared_ptr<IEventBus> bus);
    bool connect() override;
    void subscribeToSymbol(const std::string& symbol) override;
    void disconnect() override;

    // Helper method to simulate receiving a tick from the exchange
    void simulateTick(const MarketTick& tick);

private:
    std::shared_ptr<IEventBus> eventBus;
};
