#pragma once
#include <core/IEventBus.hpp>
#include <unordered_map>
#include <map>
#include <vector>
#include <string>

namespace bot::test {

// Deterministic IEventBus for unit tests: every publish is dispatched immediately on the
// caller's thread, so a test fully controls event ordering.
class SyncEventBus : public IEventBus {
public:
    uint64_t subscribeToMarketEvents(const std::string& symbol, MarketEventCallback callback) override {
        uint64_t id = nextId_++;
        marketSubs_[symbol][id] = std::move(callback);
        return id;
    }

    void unsubscribeFromMarketEvents(const std::string& symbol, uint64_t subId) override {
        auto it = marketSubs_.find(symbol);
        if (it != marketSubs_.end()) it->second.erase(subId);
    }

    void publishMarketEvent(const MarketEvent& event) override {
        std::string symbol = std::visit([](auto&& e) { return e.symbol; }, event);
        // Copy first: callbacks may (un)subscribe while being dispatched.
        auto specific = marketSubs_[symbol];
        auto wildcard = marketSubs_["*"];
        for (auto& [id, cb] : specific) cb(event);
        for (auto& [id, cb] : wildcard) cb(event);
    }

    uint64_t subscribeToExecution(ExecutionCallback callback) override {
        uint64_t id = nextId_++;
        execSubs_[id] = std::move(callback);
        return id;
    }

    void unsubscribeFromExecution(uint64_t subId) override { execSubs_.erase(subId); }

    void publishExecution(const ExecutionReport& report) override {
        auto subs = execSubs_;
        for (auto& [id, cb] : subs) cb(report);
    }

    void post(std::function<void()> task) override { task(); }
    void flush() override {}

    std::vector<HistoricalBar> fetchHistoricalBars(const std::string& symbol, const std::string&,
                                                   const std::string&, const std::string&) override {
        auto it = history_.find(symbol);
        return it != history_.end() ? it->second : std::vector<HistoricalBar>{};
    }

    // Test control
    void setHistory(const std::string& symbol, std::vector<HistoricalBar> bars) { history_[symbol] = std::move(bars); }
    size_t executionSubscriberCount() const { return execSubs_.size(); }

private:
    uint64_t nextId_ = 1;
    std::unordered_map<std::string, std::map<uint64_t, MarketEventCallback>> marketSubs_;  // ordered, like EventBus
    std::map<uint64_t, ExecutionCallback> execSubs_;
    std::unordered_map<std::string, std::vector<HistoricalBar>> history_;
};

} // namespace bot::test
