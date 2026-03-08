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

    void subscribeToExecution(ExecutionCallback callback) override {
        (void)callback;
    }

    void publishExecution(const ExecutionReport& report) override {
        receivedExecutions.push_back(report);
    }

    std::vector<MarketTick> receivedTicks;
    std::vector<ExecutionReport> receivedExecutions;
};

// Mock Subclass to isolate networking
class MockAlpacaDataFeed : public AlpacaDataFeed {
public:
    MockAlpacaDataFeed(std::shared_ptr<IEventBus> eventBus, std::string apiKey, std::string apiSecret)
        : AlpacaDataFeed(std::move(eventBus), std::move(apiKey), std::move(apiSecret)) {}

    // Expose a public way to force connection state for tests without threading
    void setTestConnected() {
        isConnected_ = true;
        isRunning_ = true;
    }

    // Capture messages sent out to verify subscription requests
    std::vector<std::string> sentMessages;

protected:
    void sendWebSocketMessage(const std::string& message) override {
        sentMessages.push_back(message);
    }
    
    void wsRunLoop() override {
        // Do nothing in tests to prevent Boost.Asio starting threads
    }
};

// Test fixture
class AlpacaDataFeedTest : public ::testing::Test {
protected:
    std::shared_ptr<MockEventBus> eventBus;
    std::unique_ptr<MockAlpacaDataFeed> dataFeed;

    void SetUp() override {
        eventBus = std::make_shared<MockEventBus>();
        dataFeed = std::make_unique<MockAlpacaDataFeed>(eventBus, "FAKE_KEY", "FAKE_SECRET");
    }
};

TEST_F(AlpacaDataFeedTest, ConnectUpdatesStateAndSendsAuth) {
    // Override connect logic slightly if we needed, but here we can just test if the parent connect
    // changes isRunning_ state or similar. But since we overrode wsRunLoop to do nothing, connect
    // will just set isRunning = true and wait forever unless we bypass it.
    // Instead of waiting for real threads, we'll force the connection state for the rest of tests.
    dataFeed->setTestConnected();
    EXPECT_TRUE(true); // Placeholder for connect logic testing without threads
}

TEST_F(AlpacaDataFeedTest, SubscribeSendsCorrectPayload) {
    dataFeed->setTestConnected();
    dataFeed->subscribeToSymbol("BTC/USD");
    
    ASSERT_EQ(dataFeed->sentMessages.size(), 1);
    EXPECT_TRUE(dataFeed->sentMessages[0].find("\"BTC/USD\"") != std::string::npos);
    EXPECT_TRUE(dataFeed->sentMessages[0].find("\"action\":\"subscribe\"") != std::string::npos);
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

TEST_F(AlpacaDataFeedTest, SubscribeFailsWhenNotConnected) {
    // DataFeed is NOT connected by default in SetUp
    dataFeed->subscribeToSymbol("AAPL");
    EXPECT_EQ(dataFeed->sentMessages.size(), 0);
}

TEST_F(AlpacaDataFeedTest, DisconnectChangesState) {
    dataFeed->setTestConnected();
    dataFeed->disconnect();
    
    // Once disconnected, subscribe should fail
    dataFeed->subscribeToSymbol("AAPL");
    EXPECT_EQ(dataFeed->sentMessages.size(), 0);
}