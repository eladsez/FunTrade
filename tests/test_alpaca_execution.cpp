#include <gtest/gtest.h>
#include <execution/AlpacaExecutionEngine.hpp>

using namespace bot;

// To make this fully testable without network, we'd ideally mock sendHttpRequest.
// Since sendHttpRequest is private but virtual, we can't easily override it unless we make it protected and subclass, 
// OR we rely on the current "Simulate successful JSON response" behavior inside sendHttpRequest.
// We will rely on the current simulated responses inside AlpacaExecutionEngine for these tests.

class AlpacaExecutionEngineTest : public ::testing::Test {
protected:
    std::unique_ptr<AlpacaExecutionEngine> engine;

    void SetUp() override {
        engine = std::make_unique<AlpacaExecutionEngine>("TEST_KEY", "TEST_SECRET", "https://paper-api.alpaca.markets");
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
