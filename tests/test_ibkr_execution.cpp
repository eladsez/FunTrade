#include <gtest/gtest.h>
#include <infra/IbkrInfra.hpp>
#include <execution/IbkrExecutionEngine.hpp>
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

class IbkrExecutionEngineTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::shared_ptr<IbkrInfra> infra;
    std::shared_ptr<IbkrExecutionEngine> engine;

    std::mutex mtx;
    std::condition_variable cv;
    std::unordered_map<std::string, OrderStatus> orderStatuses;
    std::unordered_map<std::string, std::string> clientToExchangeId;

    void SetUp() override {
        // Allow overriding connection details via env vars for CI pipelines
        const char* envHost = std::getenv("IBKR_HOST");
        const char* envPort = std::getenv("IBKR_PORT");
        
        std::string host = envHost ? envHost : "host.docker.internal"; // Or "127.0.0.1" 
        int port = envPort ? std::stoi(envPort) : 4002;     // 4002 is default Paper Gateway, 7497 is Paper TWS
        int clientId = 999; // Use a dedicated client ID for testing

        infra = std::make_shared<IbkrInfra>(host, port, clientId);
        engine = std::make_shared<IbkrExecutionEngine>(infra);
        eventBus = std::make_shared<EventBus>(nullptr, engine);

        if (!infra->connectToTws()) {
            GTEST_SKIP() << "Skipping tests: Could not connect to IBKR TWS/Gateway at " << host << ":" << port;
        }

        // Give the socket a moment to establish before hooking up engine requests
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine->connectToInfra();

        eventBus->subscribeToExecution([this](const ExecutionReport& report) {
            std::lock_guard<std::mutex> lock(mtx);
            orderStatuses[report.client_order_id] = report.status;
            clientToExchangeId[report.client_order_id] = report.order_id; 
            cv.notify_all();
        });
        
        // Wait to ensure NextValidId is received before tests start blasting orders
        std::this_thread::sleep_for(std::chrono::seconds(1)); 
    }

    void TearDown() override {
        if (engine) {
            engine->cancelAllOrders();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));    
        }
        if (infra) {
            infra->disconnectFromTws();
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
};

TEST_F(IbkrExecutionEngineTest, PlaceMarket) {
    std::string b1 = "ibkr_mkt_buy_aapl";

    // Using 1.0 quantity of US Equities
    OrderRequest buy1{b1, "AAPL", Side::BUY, OrderType::MARKET, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0};
    engine->placeOrder(buy1);

    // Note: If running this test outside of Regular Trading Hours (RTH), 
    // Market orders will remain NEW/Submitted until the open. 
    // You may need to change this assertion to OrderStatus::NEW if testing on weekends/nights.
    ASSERT_TRUE(waitForStatus({b1}, OrderStatus::FILLED) || waitForStatus({b1}, OrderStatus::NEW)) 
        << "Market buy order was neither filled nor accepted as new.";
}

TEST_F(IbkrExecutionEngineTest, CancelOrderReturnsTrue) {
    std::string l1 = "ibkr_limit_cancel_spy";

    // Place a limit buy far below market value so it doesn't fill
    OrderRequest lowLimit{l1, "SPY", Side::BUY, OrderType::LIMIT, TimeInForce::GTC, 1.0, 100.0, 0.0, 0.0};
    engine->placeOrder(lowLimit);

    ASSERT_TRUE(waitForStatus({l1}, OrderStatus::NEW))
        << "Limit order was never accepted as NEW.";

    std::string exchangeOrderId = getExchangeId(l1);
    ASSERT_FALSE(exchangeOrderId.empty()) << "Failed to capture exchange order ID.";

    engine->cancelOrder(exchangeOrderId); // Assuming your cancel uses ClientRef, otherwise use exchangeOrderId

    ASSERT_TRUE(waitForStatus({l1}, OrderStatus::CANCELED))
        << "Order was not successfully canceled.";
}

TEST_F(IbkrExecutionEngineTest, PlaceLimit) {
    std::string b1 = "ibkr_limit_buy_msft";

    // Use a low limit buy ($150) so it doesn't trigger the aggressive price collar
    OrderRequest buy1{b1, "MSFT", Side::BUY, OrderType::LIMIT, TimeInForce::GTC, 1.0, 150.0, 0.0, 0.0};
    engine->placeOrder(buy1);

    // Because it's below market, it will rest on the book as NEW, not FILLED
    ASSERT_TRUE(waitForStatus({b1}, OrderStatus::NEW))
        << "Limit buy order failed to process.";
        
    // Clean up so we don't leave resting orders on the paper account
    engine->cancelOrder(b1);
    ASSERT_TRUE(waitForStatus({b1}, OrderStatus::CANCELED));
}

TEST_F(IbkrExecutionEngineTest, PlaceTrailingStop) {
    std::string b1 = "ibkr_ts_buy_tsla";

    // 5% Trailing Stop
    OrderRequest buy1{b1, "TSLA", Side::BUY, OrderType::TRAILING_STOP, TimeInForce::GTC, 1.0, 0.0, 0.0, 0.0, 5.0};
    engine->placeOrder(buy1);

    ASSERT_TRUE(waitForStatus({b1}, OrderStatus::NEW))
        << "Trailing stop buy was not accepted.";

    engine->cancelOrder(getExchangeId(b1));
    
    ASSERT_TRUE(waitForStatus({b1}, OrderStatus::CANCELED))
        << "Failed to cancel trailing stop buy.";
}

TEST_F(IbkrExecutionEngineTest, PlaceStopLimit) {
    std::string s1 = "ibkr_stop_limit_sell_nvda";

    OrderRequest sell1{s1, "NVDA", Side::SELL, OrderType::STOP_LIMIT, TimeInForce::GTC, 1.0, 100.0, 105.0, 0.0, 0.0};
    engine->placeOrder(sell1);

    ASSERT_TRUE(waitForStatus({s1}, OrderStatus::NEW))
        << "Stop Limit sell was not accepted.";

    engine->cancelOrder(getExchangeId(s1));

    ASSERT_TRUE(waitForStatus({s1}, OrderStatus::CANCELED))
        << "Failed to clean up stop limit sell.";
}