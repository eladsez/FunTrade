#include <gtest/gtest.h>
#include <core/EventBus.hpp>
#include <data/AlpacaDataFeed.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <execution/OrderManager.hpp>
#include <risk/PortfolioManager.hpp>
#include <core/Logger.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

using namespace bot;

#ifdef PAPERTRADING
class SystemInfraStressTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::shared_ptr<AlpacaExecutionEngine> executionEngine;
    std::shared_ptr<AlpacaDataFeed> dataFeed;
    std::shared_ptr<OrderManager> orderManager;
    std::shared_ptr<PortfolioManager> portfolioManager;

    void SetUp() override {
        const char* apiKey = std::getenv("APCA_API_KEY_ID");
        const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

        ASSERT_NE(apiKey, nullptr) << "APCA_API_KEY_ID environment variable is missing.";
        ASSERT_NE(apiSecret, nullptr) << "APCA_API_SECRET_KEY environment variable is missing.";

        std::string baseUrl = "https://paper-api.alpaca.markets";
        
        eventBus = std::make_shared<EventBus>();
        executionEngine = std::make_shared<AlpacaExecutionEngine>(*eventBus, apiKey, apiSecret, baseUrl);
        dataFeed = std::make_shared<AlpacaDataFeed>(eventBus, apiKey, apiSecret);
        orderManager = std::make_shared<OrderManager>(*eventBus, *executionEngine);
        portfolioManager = std::make_shared<PortfolioManager>(*eventBus);

        ASSERT_TRUE(executionEngine->authenticate());
        ASSERT_TRUE(dataFeed->connect());
        
        // Sync initial state
        portfolioManager->syncWithExchange(*executionEngine);
        orderManager->syncWithExchange();
    }

    void TearDown() override {
        if (dataFeed) dataFeed->disconnect();
        if (eventBus) eventBus->stop();
    }
};

TEST_F(SystemInfraStressTest, FullLoopIntegrityUnderLoad) {
    std::string symbol = "BTC/USD";
    dataFeed->subscribeToSymbol(symbol);

    std::atomic<int> tickCount{0};
    eventBus->subscribeToTick(symbol, [&](const MarketTick& tick) {
        tickCount++;
        portfolioManager->onTick(tick);
    });

    LOG_INFO("[STRESS] Starting load test. Monitoring ticks and placing burst orders...");

    // Wait for some data to flow
    std::this_thread::sleep_for(std::chrono::seconds(5));
    ASSERT_GT(tickCount.load(), 0) << "No ticks received from Alpaca!";

    // Burst placement: Verify OrderManager and PortfolioManager sync
    // Alpaca Crypto has a minimum cost basis (usually $10). 
    // At $60k/BTC, 0.0002 BTC is ~$12.
    const int burstSize = 5; 
    std::vector<std::string> clientIds;

    for (int i = 0; i < burstSize; ++i) {
        OrderRequest req{"", symbol, Side::BUY, OrderType::MARKET, 0.0002};
        std::string cid = executionEngine->placeOrder(req);
        if (!cid.empty()) clientIds.push_back(cid);
        // Small delay to avoid aggressive rate limiting during burst
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("[STRESS] Placed {} orders. Waiting for fills...", clientIds.size());

    // Wait for fills (max 30s)
    auto start = std::chrono::steady_clock::now();
    bool allFilled = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        int filledCount = 0;
        for (const auto& cid : clientIds) {
            auto order = orderManager->getOrder(cid);
            if (order && order->status == OrderStatus::FILLED) {
                filledCount++;
            }
        }
        if (filledCount == (int)clientIds.size()) {
            allFilled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    EXPECT_TRUE(allFilled) << "Not all orders were filled within timeout.";

    // Final verification: Sync again and compare
    // Note: PortfolioManager now normalizes symbols to remove slashes (e.g. BTCUSD)
    std::string normalizedSymbol = symbol;
    normalizedSymbol.erase(std::remove(normalizedSymbol.begin(), normalizedSymbol.end(), '/'), normalizedSymbol.end());

    double localPos = portfolioManager->getNetPosition(normalizedSymbol);
    portfolioManager->syncWithExchange(*executionEngine);
    double remotePos = portfolioManager->getNetPosition(normalizedSymbol);

    LOG_INFO("[STRESS] Local Position: {} | Remote Position: {}", localPos, remotePos);

    // Increase epsilon to account for fee-deductions if Alpaca reports gross in reports but net in balance
    EXPECT_NEAR(localPos, remotePos, 0.0001);
    
    // Liquidate the burst to leave account clean
    if (remotePos > 0) {
         LOG_INFO("[STRESS] Cleaning up: Liquidating {} {}", remotePos, normalizedSymbol);
         OrderRequest closeReq{"", symbol, Side::SELL, OrderType::MARKET, remotePos};
         executionEngine->placeOrder(closeReq);
         std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
#endif
