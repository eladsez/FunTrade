#include <gtest/gtest.h>
#include <risk/PortfolioManager.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"

using namespace bot;
using namespace bot::test;

class PortfolioManagerTest : public ::testing::Test {
protected:
    std::shared_ptr<SyncEventBus> bus = std::make_shared<SyncEventBus>();
    std::shared_ptr<FakeExecutionEngine> engine = std::make_shared<FakeExecutionEngine>();
    std::unique_ptr<PortfolioManager> pm;

    void SetUp() override { pm = std::make_unique<PortfolioManager>(bus, engine); }

    void fill(const std::string& vkey, const std::string& sym, Side side, double qty, double px,
              OrderStatus st = OrderStatus::FILLED) {
        OrderRequest r{};
        r.client_order_id = vkey; r.symbol = sym; r.side = side; r.quantity = qty;
        bus->publishExecution(FakeExecutionEngine::report(r, st, qty, qty, px));
    }
};

TEST_F(PortfolioManagerTest, OpensLongAndShort) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_B", "B", Side::SELL, 4, 20.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_B"), -4);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 50.0);
}

TEST_F(PortfolioManagerTest, AddingUpdatesVwap) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::BUY, 10, 60.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 20);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 55.0);
}

TEST_F(PortfolioManagerTest, ReducingRealizesPnl) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::SELL, 4, 55.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 6);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->realized_pnl, 20.0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 20.0);
}

TEST_F(PortfolioManagerTest, ShortCoverRealizesPnl) {
    fill("bk_A", "A", Side::SELL, 10, 50.0);
    fill("bk_A", "A", Side::BUY, 10, 45.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 50.0);
    EXPECT_FALSE(pm->getPosition("bk_A").has_value()) << "flat positions are purged";
}

TEST_F(PortfolioManagerTest, FlipResetsAveragePrice) {
    fill("bk_A", "A", Side::BUY, 10, 50.0);
    fill("bk_A", "A", Side::SELL, 15, 52.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), -5);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk_A")->avg_price, 52.0);
    EXPECT_DOUBLE_EQ(pm->getTotalRealizedPnL(), 20.0);
}

TEST_F(PortfolioManagerTest, VirtualKeysSharingASymbolAreIndependent) {
    fill("bk1_HBAN", "HBAN", Side::BUY, 100, 15.0);
    fill("bk2_HBAN", "HBAN", Side::SELL, 60, 15.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk1_HBAN"), 100);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk2_HBAN"), -60);

    bus->publishMarketEvent(TradeTick{"HBAN", 16.0, 1, 0, Side::BUY, {}});
    EXPECT_DOUBLE_EQ(pm->getPosition("bk1_HBAN")->unrealized_pnl(), 100.0);
    EXPECT_DOUBLE_EQ(pm->getPosition("bk2_HBAN")->unrealized_pnl(), -60.0);
    EXPECT_DOUBLE_EQ(pm->getTotalUnrealizedPnL(), 40.0);
}

TEST_F(PortfolioManagerTest, StatusOnlyReportsNeverMoveInventory) {
    // IBKR orderStatus says "Filled, cumulative 10" but carries no execution.
    OrderRequest r{};
    r.client_order_id = "bk_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    for (auto st : {OrderStatus::NEW, OrderStatus::PARTIALLY_FILLED, OrderStatus::FILLED,
                    OrderStatus::CANCELED, OrderStatus::REJECTED}) {
        bus->publishExecution(FakeExecutionEngine::report(r, st, 0, 10, 50.0));
    }
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 0);
}

TEST_F(PortfolioManagerTest, LateFillAfterCancelIsStillBooked) {
    fill("bk_A", "A", Side::BUY, 10, 50.0, OrderStatus::CANCELED);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
}

TEST_F(PortfolioManagerTest, BooksUnderPositionKeyOfUniqueOrderId) {
    fill(OrderIdUtils::newClientOrderId("bk_A"), "A", Side::BUY, 10, 50.0);
    fill(OrderIdUtils::newClientOrderId("bk_A"), "A", Side::BUY, 5, 50.0);
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 15);
}

TEST_F(PortfolioManagerTest, SyncsBrokerPositionsOnStartup) {
    auto e = std::make_shared<FakeExecutionEngine>();
    e->openPositions = {Position{"AAPL", 5, 180.0, 185.0, 0.0}};
    PortfolioManager pm2(bus, e);
    EXPECT_DOUBLE_EQ(pm2.getNetPosition("AAPL"), 5);
}

// The same execution delivered twice must be counted once (fixed in Phase 2).
TEST_F(PortfolioManagerTest, DuplicateExecutionCountedOnce) {
    OrderRequest r{};
    r.client_order_id = "bk_A"; r.symbol = "A"; r.side = Side::BUY; r.quantity = 10;
    auto rep = FakeExecutionEngine::report(r, OrderStatus::FILLED, 10, 10, 50.0);
    bus->publishExecution(rep);
    bus->publishExecution(rep);  // same exec_id, redelivered
    EXPECT_DOUBLE_EQ(pm->getNetPosition("bk_A"), 10);
    EXPECT_EQ(pm->getTotalTrades(), 1u);
}

// ---------------------------------------------------------------------------
// Known bugs (see docs/ROADMAP.md). Enable when the owning phase fixes them.
// ---------------------------------------------------------------------------

// Phase 3: inventory restored from the broker must be attributed back to the basket that
// owns it, otherwise every basket restarts FLAT and orphans its hedge.
TEST_F(PortfolioManagerTest, DISABLED_Phase3_RestoredPositionsAreAttributedToBaskets) {
    auto e = std::make_shared<FakeExecutionEngine>();
    e->openPositions = {Position{"A", 10, 50.0, 50.0, 0.0}};
    PortfolioManager pm2(bus, e);
    EXPECT_DOUBLE_EQ(pm2.getNetPosition("bk_A"), 10);
}
