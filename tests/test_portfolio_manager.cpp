#include <gtest/gtest.h>
#include <risk/PortfolioManager.hpp>
#include <core/EventBus.hpp>

using namespace bot;

class PortfolioManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::unique_ptr<PortfolioManager> portfolioManager;

    void SetUp() override {
        eventBus = std::make_shared<EventBus>();
        portfolioManager = std::make_unique<PortfolioManager>(*eventBus);
        // Start the event bus to process events
        eventBus->start();
    }

    void TearDown() override {
        eventBus->stop();
    }
};

TEST_F(PortfolioManagerTest, InitialStateIsEmpty) {
    EXPECT_EQ(portfolioManager->getNetPosition("BTC/USD"), 0.0);
    EXPECT_EQ(portfolioManager->getTotalRealizedPnL(), 0.0);
    EXPECT_EQ(portfolioManager->getTotalUnrealizedPnL(), 0.0);
}

TEST_F(PortfolioManagerTest, OpenLongPosition) {
    ExecutionReport report;
    report.symbol = "BTC/USD";
    report.side = Side::BUY;
    report.status = OrderStatus::FILLED;
    report.last_quantity = 1.0;
    report.last_price = 50000.0;
    report.avg_price = 50000.0;
    report.timestamp_ms = 123456789;

    eventBus->publishExecution(report);
    
    // Give some time for background thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(portfolioManager->getNetPosition("BTC/USD"), 1.0);
    
    Position pos = portfolioManager->getPosition("BTC/USD");
    EXPECT_DOUBLE_EQ(pos.avg_price, 50000.0);
}

TEST_F(PortfolioManagerTest, IncreaseLongPosition) {
    ExecutionReport report1;
    report1.symbol = "BTC/USD";
    report1.side = Side::BUY;
    report1.status = OrderStatus::FILLED;
    report1.last_quantity = 1.0;
    report1.last_price = 50000.0;

    eventBus->publishExecution(report1);

    ExecutionReport report2;
    report2.symbol = "BTC/USD";
    report2.side = Side::BUY;
    report2.status = OrderStatus::FILLED;
    report2.last_quantity = 1.0;
    report2.last_price = 60000.0;

    eventBus->publishExecution(report2);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(portfolioManager->getNetPosition("BTC/USD"), 2.0);
    Position pos = portfolioManager->getPosition("BTC/USD");
    EXPECT_DOUBLE_EQ(pos.avg_price, 55000.0);
}

TEST_F(PortfolioManagerTest, ReduceLongPositionWithRealizedPnL) {
    // Buy 1.0 at 50000
    ExecutionReport buy;
    buy.symbol = "BTC/USD";
    buy.side = Side::BUY;
    buy.status = OrderStatus::FILLED;
    buy.last_quantity = 1.0;
    buy.last_price = 50000.0;
    eventBus->publishExecution(buy);

    // Sell 0.5 at 60000
    ExecutionReport sell;
    sell.symbol = "BTC/USD";
    sell.side = Side::SELL;
    sell.status = OrderStatus::FILLED;
    sell.last_quantity = 0.5;
    sell.last_price = 60000.0;
    eventBus->publishExecution(sell);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(portfolioManager->getNetPosition("BTC/USD"), 0.5);
    EXPECT_DOUBLE_EQ(portfolioManager->getTotalRealizedPnL(), 0.5 * (60000.0 - 50000.0));
}

TEST_F(PortfolioManagerTest, ClosePositionAndFlip) {
    // Buy 1.0 at 50000
    ExecutionReport buy;
    buy.symbol = "BTC/USD";
    buy.side = Side::BUY;
    buy.status = OrderStatus::FILLED;
    buy.last_quantity = 1.0;
    buy.last_price = 50000.0;
    eventBus->publishExecution(buy);

    // Sell 2.0 at 60000 (Close 1.0 long, open 1.0 short)
    ExecutionReport sell;
    sell.symbol = "BTC/USD";
    sell.side = Side::SELL;
    sell.status = OrderStatus::FILLED;
    sell.last_quantity = 2.0;
    sell.last_price = 60000.0;
    eventBus->publishExecution(sell);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(portfolioManager->getNetPosition("BTC/USD"), -1.0);
    EXPECT_DOUBLE_EQ(portfolioManager->getTotalRealizedPnL(), 1.0 * (60000.0 - 50000.0));
    
    Position pos = portfolioManager->getPosition("BTC/USD");
    EXPECT_DOUBLE_EQ(pos.avg_price, 60000.0);
}

TEST_F(PortfolioManagerTest, UnrealizedPnLUpdatesWithTicks) {
    // Buy 1.0 at 50000
    ExecutionReport buy;
    buy.symbol = "BTC/USD";
    buy.side = Side::BUY;
    buy.status = OrderStatus::FILLED;
    buy.last_quantity = 1.0;
    buy.last_price = 50000.0;
    eventBus->publishExecution(buy);
    
    // Give time for position to be created
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Tick at 55000
    MarketTick tick;
    tick.symbol = "BTC/USD";
    tick.price = 55000.0;
    
    portfolioManager->onTick(tick);
    
    EXPECT_DOUBLE_EQ(portfolioManager->getTotalUnrealizedPnL(), 5000.0);
}
