#include <gtest/gtest.h>
#include <data/AlpacaDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <vector>

using namespace bot;

// Mock EventBus for testing DataFeed
class MockEventBus : public IEventBus {
public:
    void subscribeToTick(const std::string& symbol, TickCallback callback) override {
        (void)symbol;
        (void)callback;
    }
    
    void publishTick(const MarketTick& tick) override {
        receivedTicks.push_back(tick);
    }

    std::vector<MarketTick> receivedTicks;
};

// Test fixture
class AlpacaDataFeedTest : public ::testing::Test {
protected:
    std::shared_ptr<MockEventBus> eventBus;
    std::unique_ptr<AlpacaDataFeed> dataFeed;

    void SetUp() override {
        eventBus = std::make_shared<MockEventBus>();
        dataFeed = std::make_unique<AlpacaDataFeed>(eventBus, "TEST_KEY", "TEST_SECRET");
    }
};

TEST_F(AlpacaDataFeedTest, ConnectSendsAuthPayload) {
    // We would need to intercept sendWebSocketMessage for a true unit test,
    // but we can at least ensure connect returns true and sets state.
    EXPECT_TRUE(dataFeed->connect());
}

TEST_F(AlpacaDataFeedTest, ParseTradeUpdateSuccessfully) {
    std::string payload = R"([
        {"T": "t", "S": "BTC/USD", "p": 55000.5, "s": 1.25, "t": 1612345678}
    ])";

    dataFeed->onWebSocketMessage(payload);

    ASSERT_EQ(eventBus->receivedTicks.size(), 1);
    EXPECT_EQ(eventBus->receivedTicks[0].symbol, "BTC/USD");
    EXPECT_DOUBLE_EQ(eventBus->receivedTicks[0].price, 55000.5);
    EXPECT_DOUBLE_EQ(eventBus->receivedTicks[0].volume, 1.25);
}

TEST_F(AlpacaDataFeedTest, ParseMultipleTradeUpdates) {
    std::string payload = R"([
        {"T": "t", "S": "BTC/USD", "p": 55000.5, "s": 1.25},
        {"T": "t", "S": "ETH/USD", "p": 3000.0, "s": 10.0}
    ])";

    dataFeed->onWebSocketMessage(payload);

    ASSERT_EQ(eventBus->receivedTicks.size(), 2);
    EXPECT_EQ(eventBus->receivedTicks[0].symbol, "BTC/USD");
    EXPECT_EQ(eventBus->receivedTicks[1].symbol, "ETH/USD");
    EXPECT_DOUBLE_EQ(eventBus->receivedTicks[1].price, 3000.0);
}

TEST_F(AlpacaDataFeedTest, IgnoreNonTradeMessages) {
    std::string payload = R"([
        {"T": "q", "S": "BTC/USD", "bp": 55000.0, "ap": 55001.0},
        {"T": "b", "S": "BTC/USD"}
    ])";

    dataFeed->onWebSocketMessage(payload);

    // Should not have published any trades
    EXPECT_EQ(eventBus->receivedTicks.size(), 0);
}

TEST_F(AlpacaDataFeedTest, HandleMalformedJsonGracefully) {
    std::string payload = "{ malformed json ";

    // Should not throw, should just catch parse error and log
    EXPECT_NO_THROW(dataFeed->onWebSocketMessage(payload));
    EXPECT_EQ(eventBus->receivedTicks.size(), 0);
}

TEST_F(AlpacaDataFeedTest, HandleMissingFieldsGracefully) {
    // Missing 'S' (symbol)
    std::string payload = R"([
        {"T": "t", "p": 55000.5, "s": 1.25}
    ])";

    // Should catch exception in parseTradeUpdate and not crash
    EXPECT_NO_THROW(dataFeed->onWebSocketMessage(payload));
    EXPECT_EQ(eventBus->receivedTicks.size(), 0);
}
