#include <gtest/gtest.h>
#include <strategy/BasketSizer.hpp>
#include "fakes/SyncEventBus.hpp"
#include "fakes/FakeExecutionEngine.hpp"
#include <cmath>
#include <numeric>

using namespace bot;
using namespace bot::test;

namespace {

double grossValue(const std::vector<LegTarget>& t, const std::vector<double>& px) {
    double v = 0.0;
    for (size_t i = 0; i < t.size(); ++i) v += t[i].quantity * px[i];
    return v;
}

} // namespace

TEST(BasketSizer, DollarExposureIsProportionalToAbsWeight) {
    std::vector<double> w{1.0, -0.36, -0.84};
    std::vector<double> px{25.0, 450.0, 80.0};
    auto t = BasketSizer::calculateEntry(PositionState::LONG, 100000.0, w, px);
    ASSERT_TRUE(t.has_value());

    double gross = grossValue(*t, px);
    double absW = std::abs(w[0]) + std::abs(w[1]) + std::abs(w[2]);
    for (size_t i = 0; i < w.size(); ++i) {
        double ratio = (*t)[i].quantity * px[i] / gross;
        EXPECT_NEAR(ratio, std::abs(w[i]) / absW, 0.025 * std::abs(w[i]) / absW) << "leg " << i;
        EXPECT_EQ((*t)[i].quantity, std::round((*t)[i].quantity)) << "whole shares only";
    }
    EXPECT_LE(gross, 100000.0 * 1.01);
}

TEST(BasketSizer, LongBuysPositiveWeightsAndSellsNegative) {
    auto t = BasketSizer::calculateEntry(PositionState::LONG, 50000.0, {1.0, -1.0}, {100.0, 100.0});
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ((*t)[0].side, Side::BUY);
    EXPECT_EQ((*t)[1].side, Side::SELL);
}

TEST(BasketSizer, ShortSellsPositiveWeightsAndBuysNegative) {
    auto t = BasketSizer::calculateEntry(PositionState::SHORT, 50000.0, {1.0, -1.0}, {100.0, 100.0});
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ((*t)[0].side, Side::SELL);
    EXPECT_EQ((*t)[1].side, Side::BUY);
}

TEST(BasketSizer, RejectsWhenCapitalCannotBuyOneShareOfTheTightestLeg) {
    // Leg 1 needs 450 / (0.36/2.2) ~= $2750 of basket capital for a single share.
    EXPECT_FALSE(BasketSizer::calculateEntry(PositionState::LONG, 2000.0, {1.0, -0.36, -0.84}, {25.0, 450.0, 80.0}));
}

TEST(BasketSizer, RejectsNonPositivePrices) {
    EXPECT_FALSE(BasketSizer::calculateEntry(PositionState::LONG, 50000.0, {1.0, -1.0}, {100.0, 0.0}));
}

TEST(BasketSizer, RejectsWhenRoundingBreaksTrackingError) {
    // Target is 50/50 of $320: leg 0 rounds 1.6 -> 2 shares ($200 vs $160), an 11% ratio error.
    EXPECT_FALSE(BasketSizer::calculateEntry(PositionState::LONG, 400.0, {1.0, -1.0}, {100.0, 160.0}));
}

TEST(BasketSizer, ExitFlattensEachVirtualPosition) {
    SyncEventBus bus;
    auto engine = std::make_shared<FakeExecutionEngine>();
    PortfolioManager pm(std::shared_ptr<IEventBus>(&bus, [](IEventBus*) {}), engine);

    OrderRequest a{}; a.client_order_id = "bk_A"; a.symbol = "A"; a.side = Side::BUY; a.quantity = 10;
    OrderRequest b{}; b.client_order_id = "bk_B"; b.symbol = "B"; b.side = Side::SELL; b.quantity = 7;
    bus.publishExecution(FakeExecutionEngine::report(a, OrderStatus::FILLED, 10, 10, 50.0));
    bus.publishExecution(FakeExecutionEngine::report(b, OrderStatus::FILLED, 7, 7, 20.0));

    auto t = BasketSizer::calculateExit({"bk_A", "bk_B", "bk_C"}, pm);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].quantity, 10); EXPECT_EQ(t[0].side, Side::SELL);
    EXPECT_EQ(t[1].quantity, 7);  EXPECT_EQ(t[1].side, Side::BUY);
    EXPECT_EQ(t[2].quantity, 0)   << "no position, nothing to trade";
}
