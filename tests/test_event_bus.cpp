#include <gtest/gtest.h>
#include <core/EventBus.hpp>
#include <atomic>
#include <chrono>

class EventBusTest : public ::testing::Test {
protected:
    std::unique_ptr<EventBus> eventBus;

    void SetUp() override {
        eventBus = std::make_unique<EventBus>();
    }

    void TearDown() override {
        eventBus->stop();
    }
};

TEST_F(EventBusTest, PublishTickDeliversToSubscriber) {
    std::atomic<int> receivedCount{0};
    double lastPrice = 0.0;

    eventBus->subscribeToTick("BTC/USD", [&](const MarketTick& tick) {
        receivedCount++;
        lastPrice = tick.price;
    });

    MarketTick tick{"BTC/USD", 50000.0, 1.0, 1000};
    eventBus->publishTick(tick);

    // Wait a short moment for the background thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(receivedCount.load(), 1);
    EXPECT_DOUBLE_EQ(lastPrice, 50000.0);
}

TEST_F(EventBusTest, MultipleSubscribersReceiveTick) {
    std::atomic<int> sub1Count{0};
    std::atomic<int> sub2Count{0};

    eventBus->subscribeToTick("ETH/USD", [&](const MarketTick& tick) {
        (void)tick;
        sub1Count++;
    });

    eventBus->subscribeToTick("ETH/USD", [&](const MarketTick& tick) {
        (void)tick;
        sub2Count++;
    });

    MarketTick tick{"ETH/USD", 3000.0, 10.0, 1000};
    eventBus->publishTick(tick);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(sub1Count.load(), 1);
    EXPECT_EQ(sub2Count.load(), 1);
}

TEST_F(EventBusTest, FilterBySymbolWorks) {
    std::atomic<int> btcCount{0};
    std::atomic<int> ethCount{0};

    eventBus->subscribeToTick("BTC/USD", [&](const MarketTick& tick) { (void)tick; btcCount++; });
    eventBus->subscribeToTick("ETH/USD", [&](const MarketTick& tick) { (void)tick; ethCount++; });

    MarketTick tick{"ETH/USD", 3000.0, 10.0, 1000};
    eventBus->publishTick(tick);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(btcCount.load(), 0);
    EXPECT_EQ(ethCount.load(), 1);
}

TEST_F(EventBusTest, GracefulShutdownProcessesRemainingEvents) {
    std::atomic<int> count{0};
    eventBus->subscribeToTick("TEST", [&](const MarketTick& tick) { (void)tick; count++; });

    // Publish some ticks
    for (int i = 0; i < 100; ++i) {
        eventBus->publishTick({"TEST", 1.0, 1.0, static_cast<uint64_t>(i)});
    }

    // Immediately stop. The destructor/stop method should drain the queue.
    eventBus->stop();

    EXPECT_EQ(count.load(), 100);
}
