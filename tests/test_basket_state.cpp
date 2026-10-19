#include <gtest/gtest.h>
#include <strategy/BasketState.hpp>
#include <cmath>
#include <numeric>
#include <vector>

using namespace bot;

namespace {

std::vector<double> sineSpreads(int n, double amp = 0.01) {
    std::vector<double> s;
    for (int i = 0; i < n; ++i) s.push_back(amp * std::sin(i * 0.7));
    return s;
}

double mean(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }

double sampleStd(const std::vector<double>& v) {
    double m = mean(v), acc = 0.0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / (v.size() - 1));
}

// Two-leg basket A - B with prices chosen so log(A) - log(B) == spread.
void setSpread(BasketState& bs, double spread, uint64_t ts) {
    bs.updatePrice("A", 100.0 * std::exp(spread), ts);
    bs.updatePrice("B", 100.0, ts);
}

} // namespace

TEST(BasketState, RejectsMismatchedSymbolsAndWeights) {
    EXPECT_THROW(BasketState(20, {"A", "B"}, {1.0}, "x"), std::invalid_argument);
}

TEST(BasketState, NoZScoreBeforeWarmUp) {
    BasketState bs(5, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : sineSpreads(5)) bs.feedHistoricalSpread(s);
    setSpread(bs, 0.0, 1000);
    EXPECT_FALSE(bs.getZScore().has_value());
}

TEST(BasketState, NoZScoreUntilEveryLegHasAPrice) {
    BasketState bs(5, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : sineSpreads(5)) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    bs.updatePrice("A", 100.0, 1000);
    EXPECT_FALSE(bs.getZScore().has_value());
}

TEST(BasketState, NoZScoreWhenLegTimestampsAreMoreThan30sApart) {
    BasketState bs(5, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : sineSpreads(5)) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    bs.updatePrice("A", 100.0, 1'000);
    bs.updatePrice("B", 100.0, 31'001);
    EXPECT_FALSE(bs.getZScore().has_value());
    bs.updatePrice("A", 100.0, 31'001);
    EXPECT_TRUE(bs.getZScore().has_value());
}

TEST(BasketState, ZScoreMatchesIndependentComputation) {
    const int W = 20;
    auto hist = sineSpreads(W);
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : hist) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);

    // First complete price set initialises the EMA to the raw spread exactly.
    const double live = 0.025;
    setSpread(bs, live, 5000);

    auto z = bs.getZScore();
    ASSERT_TRUE(z.has_value());
    EXPECT_NEAR(*z, (live - mean(hist)) / sampleStd(hist), 1e-9);
}

TEST(BasketState, SmoothsLiveSpreadWithTenPeriodEma) {
    const int W = 20;
    auto hist = sineSpreads(W);
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : hist) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);

    setSpread(bs, 0.0, 5000);       // EMA = 0 (B's update completes the price set)
    bs.updatePrice("A", 100.0 * std::exp(0.03), 5000);  // one update: EMA = 0 + (0.03 - 0) * 2/11

    double ema = 0.03 * (2.0 / 11.0);
    EXPECT_NEAR(*bs.getZScore(), (ema - mean(hist)) / sampleStd(hist), 1e-9);
}

TEST(BasketState, RollingWindowKeepsOnlyLastWSpreads) {
    const int W = 10;
    auto all = sineSpreads(W + 7);
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : all) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    setSpread(bs, 0.02, 5000);

    std::vector<double> last(all.end() - W, all.end());
    EXPECT_NEAR(*bs.getZScore(), (0.02 - mean(last)) / sampleStd(last), 1e-9);
}

TEST(BasketState, DailyCloseAdvancesWindowOnlyWhenAllLegsReported) {
    const int W = 10;
    auto hist = sineSpreads(W);
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : hist) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    setSpread(bs, 0.02, 5000);
    double before = *bs.getZScore();

    bs.updateDailyClose("A", 100.0 * std::exp(0.015));
    EXPECT_DOUBLE_EQ(*bs.getZScore(), before) << "window must not move until every leg has closed";

    bs.updateDailyClose("B", 100.0);
    std::vector<double> expected(hist.begin() + 1, hist.end());
    expected.push_back(0.015);
    EXPECT_NEAR(*bs.getZScore(), (0.02 - mean(expected)) / sampleStd(expected), 1e-9);
}

TEST(BasketState, WeightsApplyToLogPrices) {
    BasketState bs(3, {"A", "B", "C"}, {1.0, -0.5, -0.25}, "x");
    for (double s : {0.0, 0.01, -0.01}) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    bs.updatePrice("A", 50.0, 1);
    bs.updatePrice("B", 20.0, 1);
    bs.updatePrice("C", 8.0, 1);
    double spread = std::log(50.0) - 0.5 * std::log(20.0) - 0.25 * std::log(8.0);
    std::vector<double> h{0.0, 0.01, -0.01};
    EXPECT_NEAR(*bs.getZScore(), (spread - mean(h)) / sampleStd(h), 1e-9);
}

// ---------------------------------------------------------------------------
// Known bugs (see docs/ROADMAP.md). Enable when the owning phase fixes them.
// ---------------------------------------------------------------------------

// Phase 4: a window that was never filled (empty/short REST history) must not produce a
// tradeable z-score. Today the zero-filled slots make the first daily close look like |z|~6.
TEST(BasketStateKnownBugs, DISABLED_Phase4_NoZScoreUntilWindowIsFull) {
    BasketState bs(38, {"A", "B"}, {1.0, -1.0}, "x");
    bs.setWarmedUp(true);
    setSpread(bs, 0.0, 1000);
    bs.updateDailyClose("A", 100.0);
    bs.updateDailyClose("B", 100.0);
    EXPECT_FALSE(bs.getZScore().has_value());
}

// Phase 4: several daily-bar updates on the same trading day must advance the window once.
TEST(BasketStateKnownBugs, DISABLED_Phase4_RepeatedSameDayClosesAdvanceWindowOnce) {
    const int W = 10;
    auto hist = sineSpreads(W);
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    for (double s : hist) bs.feedHistoricalSpread(s);
    bs.setWarmedUp(true);
    setSpread(bs, 0.02, 5000);
    for (int i = 0; i < 5; ++i) {
        bs.updateDailyClose("A", 100.0 * std::exp(0.015));
        bs.updateDailyClose("B", 100.0);
    }
    std::vector<double> expected(hist.begin() + 1, hist.end());
    expected.push_back(0.015);
    EXPECT_NEAR(*bs.getZScore(), (0.02 - mean(expected)) / sampleStd(expected), 1e-9);
}
