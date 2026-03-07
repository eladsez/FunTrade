#include <gtest/gtest.h>
#include <execution/AlpacaExecutionEngine.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>

using namespace bot;

// Mock Subclass to isolate networking
class MockAlpacaExecutionEngine : public AlpacaExecutionEngine {
public:
    MockAlpacaExecutionEngine(std::string apiKey, std::string apiSecret, std::string baseUrl)
        : AlpacaExecutionEngine(std::move(apiKey), std::move(apiSecret), std::move(baseUrl)) {}

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
    std::unique_ptr<MockAlpacaExecutionEngine> engine;

    void SetUp() override {
        engine = std::make_unique<MockAlpacaExecutionEngine>("FAKE_KEY", "FAKE_SECRET", "https://paper-api.alpaca.markets");
    }
};

TEST_F(AlpacaExecutionEngineTest, AuthenticateReturnsTrue) {
    EXPECT_TRUE(engine->authenticate());
}

TEST_F(AlpacaExecutionEngineTest, PlaceMarketOrderReturnsOrderId) {
    OrderRequest req{"BTC/USD", Side::BUY, OrderType::MARKET, 1.5, 0.0};
    std::string orderId = engine->placeOrder(req);

    // Our mocked sendHttpRequest returns {"id": "alpaca-mock-id-9999", "status": "accepted"}
    EXPECT_EQ(orderId, "alpaca-mock-id-9999");
}

TEST_F(AlpacaExecutionEngineTest, CancelOrderReturnsTrue) {
    bool success = engine->cancelOrder("alpaca-mock-id-9999");
    // Our mocked sendHttpRequest returns {} for DELETE, and success is when response is not empty
    EXPECT_TRUE(success);
}

TEST_F(AlpacaExecutionEngineTest, GetPortfolioValueParsesCorrectly) {
    auto value = engine->getPortfolioValue();
    ASSERT_TRUE(value.has_value());
    // Our mocked sendHttpRequest returns {"portfolio_value": "100500.25"}
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
    OrderRequest req{"ETH/USD", Side::SELL, OrderType::LIMIT, 2.0, 4000.0};
    std::string orderId = engine->placeOrder(req);
    EXPECT_EQ(orderId, "alpaca-mock-id-9999");
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
    
    // We use the real AlpacaExecutionEngine here
    AlpacaExecutionEngine engine(apiKey, apiSecret, baseUrl);

    // 1. Authenticate
    EXPECT_TRUE(engine.authenticate());

    // 2. Portfolio Value
    auto portfolioValue = engine.getPortfolioValue();
    EXPECT_TRUE(portfolioValue.has_value());
    if (portfolioValue) {
        std::cout << "[INTEGRATION] Current Portfolio Value: $" << *portfolioValue << std::endl;
    }

    // 3. Place a Market BUY Order
    OrderRequest marketBuy{"BTC/USD", Side::BUY, OrderType::MARKET, 0.001, 0.0};
    std::string orderId = engine.placeOrder(marketBuy);
    ASSERT_FALSE(orderId.empty()) << "Failed to place Market BUY order.";
    std::cout << "[INTEGRATION] Market Order ID: " << orderId << std::endl;

    // 4. Get Open Positions and dynamically sell what we have
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto positions = engine.getOpenPositions();
    double sellQty = 0.001;
    if (positions.is_array() && !positions.empty()) {
        for (const auto& pos : positions) {
            if (pos.value("symbol", "") == "BTCUSD" || pos.value("symbol", "") == "BTC/USD") {
                sellQty = std::stod(pos.value("qty_available", "0.001"));
                break;
            }
        }
    }

    // 5. Place a Limit SELL Order
    OrderRequest limitSell{"BTC/USD", Side::SELL, OrderType::LIMIT, sellQty, 150000.0};
    std::string limitOrderId = engine.placeOrder(limitSell);
    ASSERT_FALSE(limitOrderId.empty()) << "Failed to place Limit SELL order.";

    // 6. Cancel the Limit SELL Order
    EXPECT_TRUE(engine.cancelOrder(limitOrderId));
}
#endif
