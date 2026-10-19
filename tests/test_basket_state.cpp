#include <gtest/gtest.h>
#include <strategy/BasketState.hpp>
#include <cmath>
#include <numeric>
#include <vector>

using namespace bot;

namespace {

double mean(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }

double sampleStd(const std::vector<double>& v) {
    double m = mean(v), acc = 0.0;
    for (double x : v) acc += (x - m) * (x - m);
    return std::sqrt(acc / (v.size() - 1));
}

double spreadAt(int i) { return 0.01 * std::sin(i * 0.7); }

// Two-leg basket A - B: closes chosen so log(A) - log(B) == spread.
void setClose(BasketState& bs, int64_t day, double spread) {
    bs.setDailyClose("A", day, 100.0 * std::exp(spread));
    bs.setDailyClose("B", day, 100.0);
}

// Research reference: z of the last value over a window that includes it (pandas rolling).
double researchZ(const std::vector<double>& window) {
    return (window.back() - mean(window)) / sampleStd(window);
}

} // namespace

TEST(BasketState, RejectsBadConstruction) {
    EXPECT_THROW(BasketState(20, {"A", "B"}, {1.0}, "x"), std::invalid_argument);
    EXPECT_THROW(BasketState(1, {"A"}, {1.0}, "x"), std::invalid_argument);
}

TEST(BasketState, DayIsCompleteOnlyWhenEveryLegHasAClose) {
    BasketState bs(5, {"A", "B"}, {1.0, -1.0}, "x");
    bs.setDailyClose("A", 100, 50.0);
    EXPECT_FALSE(bs.isDayComplete(100));
    bs.setDailyClose("B", 100, 20.0);
    EXPECT_TRUE(bs.isDayComplete(100));
    EXPECT_EQ(bs.latestCompleteDay(), 100);
}

TEST(BasketState, ZScoreMatchesResearchFormula) {
    const int W = 20;
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    std::vector<double> spreads;
    for (int d = 0; d < W; ++d) { setClose(bs, 1000 + d, spreadAt(d)); spreads.push_back(spreadAt(d)); }

    auto z = bs.dailyZScore(1000 + W - 1);
    ASSERT_TRUE(z);
    EXPECT_NEAR(*z, researchZ(spreads), 1e-9) << "window includes the day itself, sample std";
}

TEST(BasketState, WindowRollsOverLastWCompleteDays) {
    const int W = 10;
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    std::vector<double> all;
    for (int d = 0; d < W + 7; ++d) { setClose(bs, 1000 + d, spreadAt(d)); all.push_back(spreadAt(d)); }
    std::vector<double> last(all.end() - W, all.end());
    EXPECT_NEAR(*bs.dailyZScore(1000 + W + 6), researchZ(last), 1e-9);

    // And historical days can be evaluated too (window ending at that day).
    std::vector<double> earlier(all.begin() + 2, all.begin() + 2 + W);
    EXPECT_NEAR(*bs.dailyZScore(1000 + W + 1), researchZ(earlier), 1e-9);
}

TEST(BasketState, NoZScoreUntilWindowIsFull) {  // was a Phase 4 known bug
    BasketState bs(38, {"A", "B"}, {1.0, -1.0}, "x");
    for (int d = 0; d < 37; ++d) setClose(bs, 1000 + d, spreadAt(d));
    EXPECT_FALSE(bs.dailyZScore(1036));
    setClose(bs, 1037, spreadAt(37));
    EXPECT_TRUE(bs.dailyZScore(1037));
}

TEST(BasketState, RepeatedSameDayClosesOverwriteInsteadOfAdvancing) {  // was a Phase 4 known bug
    const int W = 10;
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    std::vector<double> spreads;
    for (int d = 0; d < W; ++d) { setClose(bs, 1000 + d, spreadAt(d)); spreads.push_back(spreadAt(d)); }
    for (int i = 0; i < 5; ++i) setClose(bs, 1000 + W - 1, 0.02 + i * 0.001);  // intraday updates of the same day
    spreads.back() = 0.024;
    EXPECT_NEAR(*bs.dailyZScore(1000 + W - 1), researchZ(spreads), 1e-9);
    EXPECT_EQ(bs.completeDayCount(), static_cast<size_t>(W));
}

TEST(BasketState, IncompleteDaysAreSkippedInTheWindow) {
    const int W = 5;
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    std::vector<double> spreads;
    for (int d = 0; d < W; ++d) { setClose(bs, 1000 + 2 * d, spreadAt(d)); spreads.push_back(spreadAt(d)); }
    bs.setDailyClose("A", 1001, 999.0);  // a day with only one leg must not enter the window
    EXPECT_NEAR(*bs.dailyZScore(1000 + 2 * (W - 1)), researchZ(spreads), 1e-9);
}

TEST(BasketState, NoZScoreForAnIncompleteDay) {
    BasketState bs(3, {"A", "B"}, {1.0, -1.0}, "x");
    for (int d = 0; d < 3; ++d) setClose(bs, 1000 + d, spreadAt(d));
    bs.setDailyClose("A", 1003, 101.0);
    EXPECT_FALSE(bs.dailyZScore(1003));
}

TEST(BasketState, FlatSpreadHasNoSignal) {
    BasketState bs(5, {"A", "B"}, {1.0, -1.0}, "x");
    for (int d = 0; d < 5; ++d) setClose(bs, 1000 + d, 0.0);
    EXPECT_FALSE(bs.dailyZScore(1004));
}

TEST(BasketState, WeightsApplyToLogPrices) {
    BasketState bs(3, {"A", "B", "C"}, {1.0, -0.5, -0.25}, "x");
    std::vector<std::vector<double>> px{{50, 20, 8}, {51, 20, 8}, {50, 21, 9}};
    std::vector<double> spreads;
    for (int d = 0; d < 3; ++d) {
        bs.setDailyClose("A", 1000 + d, px[d][0]);
        bs.setDailyClose("B", 1000 + d, px[d][1]);
        bs.setDailyClose("C", 1000 + d, px[d][2]);
        spreads.push_back(std::log(px[d][0]) - 0.5 * std::log(px[d][1]) - 0.25 * std::log(px[d][2]));
    }
    EXPECT_NEAR(*bs.dailyZScore(1002), researchZ(spreads), 1e-9);
}

TEST(BasketState, IndicativeZUsesLivePricesAgainstLatestWindow) {
    const int W = 10;
    BasketState bs(W, {"A", "B"}, {1.0, -1.0}, "x");
    std::vector<double> spreads;
    for (int d = 0; d < W; ++d) { setClose(bs, 1000 + d, spreadAt(d)); spreads.push_back(spreadAt(d)); }
    EXPECT_FALSE(bs.indicativeZScore()) << "no live prices yet";
    bs.updatePrice("A", 100.0 * std::exp(0.03), 1);
    bs.updatePrice("B", 100.0, 1);
    EXPECT_NEAR(*bs.indicativeZScore(), (0.03 - mean(spreads)) / sampleStd(spreads), 1e-9);
}
