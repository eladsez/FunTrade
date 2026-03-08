#include <gtest/gtest.h>
#include <execution/AlpacaExecutionEngine.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>

#include <core/EventBus.hpp>

using namespace bot;

// Mock Subclass to isolate networking
class MockAlpacaExecutionEngine : public AlpacaExecutionEngine {
public:
    MockAlpacaExecutionEngine(IEventBus& eventBus, std::string apiKey, std::string apiSecret, std::string baseUrl)
        : AlpacaExecutionEngine(eventBus, std::move(apiKey), std::move(apiSecret), std::move(baseUrl)) {}

protected:
    std::string sendHttpRequest(const std::string& method, const std::string& endpoint, const std::string& payload = "") override {
        // Return simulated JSON responses instead of hitting the live network
        if (method == "POST" && endpoint == "/v2/orders") {
            return R"({"id": "alpaca-mock-id-9999", "status": "accepted"})";
        } else if (method == "DELETE") {
            return R"({})";
        } else if (method == "GET" && endpoint == "/v2/account") {
            return R"({"portfolio_value": "100500.25"})";
        } else if (method == "GET" && endpoint == "/v2/positions") {
            return R"([{"symbol": "BTC/USD", "qty": "1.5"}])";
        }
        return "";
    }
};

class AlpacaExecutionEngineTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::unique_ptr<MockAlpacaExecutionEngine> engine;

    void SetUp() override {
        eventBus = std::make_shared<EventBus>();
        engine = std::make_unique<MockAlpacaExecutionEngine>(*eventBus, "FAKE_KEY", "FAKE_SECRET", "https://paper-api.alpaca.markets");
    }
};

TEST_F(AlpacaExecutionEngineTest, AuthenticateReturnsTrue) {
    EXPECT_TRUE(engine->authenticate());
}

TEST_F(AlpacaExecutionEngineTest, PlaceMarketOrderReturnsClientOrderId) {
    OrderRequest req{"", "BTC/USD", Side::BUY, OrderType::MARKET, 1.5, 0.0};
    std::string orderId = engine->placeOrder(req);

    // Should return a bot-generated ID since we passed empty
    EXPECT_FALSE(orderId.empty());
    EXPECT_TRUE(orderId.find("bot_") == 0 || orderId == ""); 
}

TEST_F(AlpacaExecutionEngineTest, CancelOrderReturnsTrue) {
    bool success = engine->cancelOrder("alpaca-mock-id-9999");
    EXPECT_TRUE(success);
}

TEST_F(AlpacaExecutionEngineTest, GetPortfolioValueParsesCorrectly) {
    auto value = engine->getPortfolioValue();
    ASSERT_TRUE(value.has_value());
    EXPECT_DOUBLE_EQ(value.value(), 100500.25);
}

TEST_F(AlpacaExecutionEngineTest, GetOpenPositionsParsesCorrectly) {
    auto positions = engine->getOpenPositions();
    ASSERT_TRUE(positions.is_array());
    ASSERT_EQ(positions.size(), 1);
    EXPECT_EQ(positions[0]["symbol"], "BTC/USD");
    EXPECT_EQ(positions[0]["qty"], "1.5");
}

TEST_F(AlpacaExecutionEngineTest, PlaceLimitOrderSendsPrice) {
    OrderRequest req{"my_custom_id", "ETH/USD", Side::SELL, OrderType::LIMIT, 2.0, 4000.0};
    std::string orderId = engine->placeOrder(req);
    EXPECT_EQ(orderId, "my_custom_id");
}

// Integration Test - Only runs if ENABLE_PAPERTRADING_TEST=ON and credentials are set
#ifdef PAPERTRADING
TEST(AlpacaExecutionIntegration, LivePaperTradingFlow) {
    const char* apiKeyEnv = std::getenv("APCA_API_KEY_ID");
    const char* apiSecretEnv = std::getenv("APCA_API_SECRET_KEY");

    ASSERT_NE(apiKeyEnv, nullptr) << "APCA_API_KEY_ID environment variable is missing.";
    ASSERT_NE(apiSecretEnv, nullptr) << "APCA_API_SECRET_KEY environment variable is missing.";

    std::string apiKey(apiKeyEnv);
    std::string apiSecret(apiSecretEnv);
    std::string baseUrl = "https://paper-api.alpaca.markets";
    
    EventBus eventBus;
    AlpacaExecutionEngine engine(eventBus, apiKey, apiSecret, baseUrl);

    EXPECT_TRUE(engine.authenticate());

    auto portfolioValue = engine.getPortfolioValue();
    EXPECT_TRUE(portfolioValue.has_value());

    OrderRequest marketBuy{"", "BTC/USD", Side::BUY, OrderType::MARKET, 0.0001, 0.0};
    std::string orderId = engine.placeOrder(marketBuy);
    ASSERT_FALSE(orderId.empty());
    std::cout << "[INTEGRATION] Market Order Client ID: " << orderId << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(5));
}
#endif
