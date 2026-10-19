#include <gtest/gtest.h>
#include <core/EventBus.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace bot;

namespace {

// Waits (bounded) for the async dispatcher to deliver `n` events.
template <typename F>
bool waitUntil(F pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

MarketEvent trade(const std::string& sym, double px) { return TradeTick{sym, px, 1, 0, Side::BUY, {}}; }

} // namespace

class EventBusTest : public ::testing::Test {
protected:
    std::unique_ptr<EventBus> bus = std::make_unique<EventBus>(nullptr, nullptr);
};

TEST_F(EventBusTest, DeliversOnlySubscribedSymbol) {
    std::atomic<int> a{0}, b{0};
    bus->subscribeToMarketEvents("A", [&](const MarketEvent&) { a++; });
    bus->subscribeToMarketEvents("B", [&](const MarketEvent&) { b++; });
    bus->publishMarketEvent(trade("A", 1));
    bus->publishMarketEvent(trade("A", 2));
    bus->publishMarketEvent(trade("C", 3));
    ASSERT_TRUE(waitUntil([&] { return a == 2; }));
    bus->stop();  // drains the queue
    EXPECT_EQ(b, 0);
}

TEST_F(EventBusTest, WildcardReceivesEverySymbol) {
    std::atomic<int> n{0};
    bus->subscribeToMarketEvents("*", [&](const MarketEvent&) { n++; });
    bus->publishMarketEvent(trade("A", 1));
    bus->publishMarketEvent(trade("Z", 1));
    EXPECT_TRUE(waitUntil([&] { return n == 2; }));
}

TEST_F(EventBusTest, UnsubscribeStopsDelivery) {
    std::atomic<int> n{0};
    auto id = bus->subscribeToMarketEvents("A", [&](const MarketEvent&) { n++; });
    bus->publishMarketEvent(trade("A", 1));
    ASSERT_TRUE(waitUntil([&] { return n == 1; }));
    bus->unsubscribeFromMarketEvents("A", id);
    bus->publishMarketEvent(trade("A", 2));
    bus->stop();
    EXPECT_EQ(n, 1);
}

TEST_F(EventBusTest, PreservesOrderAcrossMarketAndExecutionEvents) {
    std::mutex m;
    std::vector<std::string> seen;
    bus->subscribeToMarketEvents("A", [&](const MarketEvent& e) {
        std::lock_guard l(m); seen.push_back("m" + std::to_string((int)std::get<TradeTick>(e).price));
    });
    bus->subscribeToExecution([&](const ExecutionReport& r) {
        std::lock_guard l(m); seen.push_back("x" + r.client_order_id);
    });
    for (int i = 0; i < 50; ++i) {
        bus->publishMarketEvent(trade("A", i));
        ExecutionReport r{}; r.client_order_id = std::to_string(i);
        bus->publishExecution(r);
    }
    bus->stop();
    ASSERT_EQ(seen.size(), 100u);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(seen[2 * i], "m" + std::to_string(i));
        EXPECT_EQ(seen[2 * i + 1], "x" + std::to_string(i));
    }
}

TEST_F(EventBusTest, UnsubscribeExecutionStopsDelivery) {
    std::atomic<int> n{0};
    auto id = bus->subscribeToExecution([&](const ExecutionReport&) { n++; });
    bus->unsubscribeFromExecution(id);
    bus->publishExecution(ExecutionReport{});
    bus->stop();
    EXPECT_EQ(n, 0);
}
