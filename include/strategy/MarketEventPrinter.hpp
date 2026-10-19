#pragma once
#include <strategy/IStrategy.hpp>
#include <core/IEventBus.hpp>
#include <execution/IExecutionEngine.hpp>
#include <risk/PortfolioManager.hpp>
#include <memory>

namespace bot {

class MarketEventPrinter : public IStrategy {
public:
    MarketEventPrinter(std::shared_ptr<IEventBus> bus, const std::string& symbol);
    void start() override;
    virtual void onMarketEvent(const MarketEvent& event);
    void stop() override;

private:
    std::shared_ptr<IEventBus> eventBus;
    std::string symbol_;
    uint64_t sub_id_;
};

} // namespace bot
