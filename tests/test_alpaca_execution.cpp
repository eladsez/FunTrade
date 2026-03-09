#include <gtest/gtest.h>
#include <execution/AlpacaExecutionEngine.hpp>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <atomic>

#include <core/EventBus.hpp>

using namespace bot;

class AlpacaExecutionEngineTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::unique_ptr<AlpacaExecutionEngine> engine;

    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<std::string, OrderStatus> orderStatuses;
    std::unordered_map<std::string, std::string> clientToExchangeId;

    void SetUp() override {
        const char* apiKey = std::getenv("APCA_API_KEY_ID");
        const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

        if (!apiKey || !apiSecret) {
            GTEST_SKIP() << "Skipping tests: APCA_API_KEY_ID or APCA_API_SECRET_KEY not found in environment.";
        }

        eventBus = std::make_shared<EventBus>();
        engine = std::make_unique<AlpacaExecutionEngine>(eventBus, apiKey, apiSecret);

        eventBus->start();

        eventBus->subscribeToExecution([this](const ExecutionReport& report) {
            std::lock_guard<std::mutex> lock(mtx);
            orderStatuses[report.client_order_id] = report.status;
            clientToExchangeId[report.client_order_id] = report.order_id; 
            cv.notify_all();
        });
    }

    void TearDown() override {
        if (engine) {
            // The ultimate cleanup: Wipe all resting orders so the next test starts with a clean slate
            engine->cancelAllOrders();
            
            // Give the exchange a moment to process the mass-cancel before disconnecting
            std::this_thread::sleep_for(std::chrono::milliseconds(500));    
        }
    }

    bool waitForStatus(const std::vector<std::string>& clientIds, OrderStatus expected, int timeoutSeconds = 15) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::seconds(timeoutSeconds), [this, &clientIds, expected]() {
            for (const auto& cid : clientIds) {
                auto it = orderStatuses.find(cid);
                if (it == orderStatuses.end() || it->second != expected) {
                    return false; 
                }
            }
            return true; 
        });
    }

    std::string getExchangeId(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = clientToExchangeId.find(clientId);
        return (it != clientToExchangeId.end()) ? it->second : "";
    }

    // Helper to guarantee unique client order IDs on every test run
    std::string genId(const std::string& prefix) {
        static std::atomic<int> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return prefix + "_" + std::to_string(now) + "_" + std::to_string(++counter);
    }
};

TEST_F(AlpacaExecutionEngineTest, PlaceMarket) {
    std::string b1 = genId("mkt_buy_btc_1");
    std::string b2 = genId("mkt_buy_btc_2");

    OrderRequest buy1{b1, "BTC/USD", Side::BUY, OrderType::MARKET, TimeInForce::GTC, 0.01, 0.0, 0.0, 0.0};
    OrderRequest buy2{b2, "BTC/USD", Side::BUY, OrderType::MARKET, TimeInForce::GTC, 0.01, 0.0, 0.0, 0.0};

    engine->placeOrder(buy1);
    engine->placeOrder(buy2);

    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::FILLED)) 
        << "Market buy orders did not fill in time.";

    std::string s1 = genId("mkt_sell_btc_1");
    std::string s2 = genId("mkt_sell_btc_2");

    OrderRequest sell1{s1, "BTC/USD", Side::SELL, OrderType::MARKET, TimeInForce::GTC, 0.005, 0.0, 0.0, 0.0};
    OrderRequest sell2{s2, "BTC/USD", Side::SELL, OrderType::MARKET, TimeInForce::GTC, 0.005, 0.0, 0.0, 0.0};

    engine->placeOrder(sell1);
    engine->placeOrder(sell2);

    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::FILLED))
        << "Market sell orders did not fill in time.";
}

TEST_F(AlpacaExecutionEngineTest, CancelOrderReturnsTrue) {
    std::string l1 = genId("limit_cancel_eth");

    OrderRequest lowLimit{l1, "ETH/USD", Side::BUY, OrderType::LIMIT, TimeInForce::GTC, 1.0, 100.0, 0.0, 0.0};
    engine->placeOrder(lowLimit);

    ASSERT_TRUE(waitForStatus({l1}, OrderStatus::NEW))
        << "Limit order was never accepted as NEW.";

    std::string exchangeOrderId = getExchangeId(l1);
    ASSERT_FALSE(exchangeOrderId.empty()) << "Failed to capture exchange order ID.";

    engine->cancelOrder(exchangeOrderId);

    ASSERT_TRUE(waitForStatus({l1}, OrderStatus::CANCELED))
        << "Order was not successfully canceled.";
}

TEST_F(AlpacaExecutionEngineTest, PlaceLimit) {
    std::string b1 = genId("limit_buy_eth_1");
    std::string b2 = genId("limit_buy_eth_2");

    OrderRequest buy1{b1, "ETH/USD", Side::BUY, OrderType::LIMIT, TimeInForce::GTC, 0.1, 200000.0, 0.0, 0.0};
    OrderRequest buy2{b2, "ETH/USD", Side::BUY, OrderType::LIMIT, TimeInForce::GTC, 0.1, 200000.0, 0.0, 0.0};

    engine->placeOrder(buy1);
    engine->placeOrder(buy2);

    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::FILLED))
        << "High-priced limit buy orders did not fill instantly.";

    std::string s1 = genId("limit_sell_eth_1");
    std::string s2 = genId("limit_sell_eth_2");

    OrderRequest sell1{s1, "ETH/USD", Side::SELL, OrderType::LIMIT, TimeInForce::GTC, 0.05, 10.0, 0.0, 0.0};
    OrderRequest sell2{s2, "ETH/USD", Side::SELL, OrderType::LIMIT, TimeInForce::GTC, 0.05, 10.0, 0.0, 0.0};

    engine->placeOrder(sell1);
    engine->placeOrder(sell2);

    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::FILLED))
        << "Low-priced limit sell orders did not fill instantly.";
}

TEST_F(AlpacaExecutionEngineTest, PlaceTrailingStop) {
    std::string b1 = genId("ts_buy_aapl_1");
    std::string b2 = genId("ts_buy_aapl_2");

    OrderRequest buy1{b1, "AAPL", Side::BUY, OrderType::TRAILING_STOP, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0, 5.0};
    OrderRequest buy2{b2, "AAPL", Side::BUY, OrderType::TRAILING_STOP, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0, 5.0};

    engine->placeOrder(buy1);
    engine->placeOrder(buy2);

    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::NEW))
        << "Trailing stop buys were not accepted.";

    // Cancel the pending buys BEFORE placing sells to avoid Alpaca's long/short conflict rule
    engine->cancelOrder(getExchangeId(b1));
    engine->cancelOrder(getExchangeId(b2));
    
    // Wait for the cancellations to clear the exchange risk engine
    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::CANCELED))
        << "Failed to cancel trailing stop buys.";

    std::string s1 = genId("ts_sell_aapl_1");
    std::string s2 = genId("ts_sell_aapl_2");

    OrderRequest sell1{s1, "AAPL", Side::SELL, OrderType::TRAILING_STOP, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0, 5.0};
    OrderRequest sell2{s2, "AAPL", Side::SELL, OrderType::TRAILING_STOP, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0, 5.0};

    engine->placeOrder(sell1);
    engine->placeOrder(sell2);

    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::NEW))
        << "Trailing stop sells were not accepted.";

    engine->cancelOrder(getExchangeId(s1));
    engine->cancelOrder(getExchangeId(s2));

    // MUST wait for the cancels to process before the test exits
    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::CANCELED))
        << "Failed to clean up trailing stop sells.";
}

TEST_F(AlpacaExecutionEngineTest, PlaceStop) {
    std::string b1 = genId("stop_buy_msft_1");
    std::string b2 = genId("stop_buy_msft_2");

    // Use MSFT to completely isolate from the previous test
    OrderRequest buy1{b1, "MSFT", Side::BUY, OrderType::STOP, TimeInForce::GTC, 1.0, 0.0, 450.0, 0.0, 0.0};
    OrderRequest buy2{b2, "MSFT", Side::BUY, OrderType::STOP, TimeInForce::GTC, 1.0, 0.0, 450.0, 0.0, 0.0};

    engine->placeOrder(buy1);
    engine->placeOrder(buy2);

    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::NEW))
        << "Stop buys were not accepted.";

    engine->cancelOrder(getExchangeId(b1));
    engine->cancelOrder(getExchangeId(b2));
    
    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::CANCELED))
        << "Failed to cancel stop buys.";

    std::string s1 = genId("stop_sell_msft_1");
    std::string s2 = genId("stop_sell_msft_2");

    OrderRequest sell1{s1, "MSFT", Side::SELL, OrderType::STOP, TimeInForce::GTC, 1.0, 0.0, 350.0, 0.0, 0.0};
    OrderRequest sell2{s2, "MSFT", Side::SELL, OrderType::STOP, TimeInForce::GTC, 1.0, 0.0, 350.0, 0.0, 0.0};

    engine->placeOrder(sell1);
    engine->placeOrder(sell2);

    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::NEW))
        << "Stop sells were not accepted.";

    engine->cancelOrder(getExchangeId(s1));
    engine->cancelOrder(getExchangeId(s2));

    // Wait for cleanup
    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::CANCELED))
        << "Failed to clean up stop sells.";
}

TEST_F(AlpacaExecutionEngineTest, PlaceStopLimit) {
    std::string b1 = genId("stop_limit_buy_tsla_1");
    std::string b2 = genId("stop_limit_buy_tsla_2");

    // Use TSLA for isolation
    OrderRequest buy1{b1, "TSLA", Side::BUY, OrderType::STOP_LIMIT, TimeInForce::GTC, 1.0, 255.0, 250.0, 0.0, 0.0};
    OrderRequest buy2{b2, "TSLA", Side::BUY, OrderType::STOP_LIMIT, TimeInForce::GTC, 1.0, 255.0, 250.0, 0.0, 0.0};

    engine->placeOrder(buy1);
    engine->placeOrder(buy2);

    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::NEW))
        << "Stop Limit buys were not accepted.";

    engine->cancelOrder(getExchangeId(b1));
    engine->cancelOrder(getExchangeId(b2));
    
    ASSERT_TRUE(waitForStatus({b1, b2}, OrderStatus::CANCELED))
        << "Failed to cancel Stop Limit buys.";

    std::string s1 = genId("stop_limit_sell_tsla_1");
    std::string s2 = genId("stop_limit_sell_tsla_2");

    OrderRequest sell1{s1, "TSLA", Side::SELL, OrderType::STOP_LIMIT, TimeInForce::GTC, 1.0, 145.0, 150.0, 0.0, 0.0};
    OrderRequest sell2{s2, "TSLA", Side::SELL, OrderType::STOP_LIMIT, TimeInForce::GTC, 1.0, 145.0, 150.0, 0.0, 0.0};

    engine->placeOrder(sell1);
    engine->placeOrder(sell2);

    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::NEW))
        << "Stop Limit sells were not accepted.";

    engine->cancelOrder(getExchangeId(s1));
    engine->cancelOrder(getExchangeId(s2));

    // Wait for cleanup
    ASSERT_TRUE(waitForStatus({s1, s2}, OrderStatus::CANCELED))
        << "Failed to clean up stop limit sells.";
}