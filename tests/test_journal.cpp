#include <gtest/gtest.h>
#include "fakes/StrategyHarness.hpp"
#include <utils/Journal.hpp>
#include <fstream>

using namespace bot;
using namespace bot::test;

// The dashboard's history (z per close, cycles, fills) comes only from the journal.
class JournalTest : public StrategyHarness {
protected:
    std::filesystem::path file;

    void SetUp() override {
        file = tmp / "journal.jsonl";
        Journal::instance().open(file);
        build();
        start();
    }

    void TearDown() override {
        Journal::instance().close();
        StrategyHarness::TearDown();
    }

    std::vector<nlohmann::json> events(const std::string& type) const {
        std::vector<nlohmann::json> out;
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            auto j = nlohmann::json::parse(line);
            if (j.at("type") == type) out.push_back(j);
        }
        return out;
    }
};

TEST_F(JournalTest, RecordsEachCloseDecisionWithItsZ) {
    size_t before = events("close").size();  // start() decides on the last history day
    closeDay(0.0);
    closeDay(0.05);
    auto all = events("close");
    ASSERT_EQ(all.size(), before + 2);
    std::vector<nlohmann::json> closes(all.end() - 2, all.end());
    EXPECT_EQ(closes[1]["basket"], "tb");
    EXPECT_EQ(closes[1]["day"], day);
    EXPECT_EQ(closes[1]["action"], "ENTER_SHORT");
    EXPECT_EQ(closes[0]["action"], "NONE");
    ASSERT_TRUE(closes[1]["z"].is_number());
    EXPECT_DOUBLE_EQ(closes[1]["z"].get<double>(), strat->lastDailyZ().value());
    EXPECT_GT(closes[1]["t"].get<int64_t>(), 0);
}

TEST_F(JournalTest, RecordsCycleLegsFillsAndOutcome) {
    closeDay(0.05);
    openSession();
    auto starts = events("cycle_start");
    ASSERT_EQ(starts.size(), 1u);
    EXPECT_EQ(starts[0]["label"], "ENTRY");
    EXPECT_EQ(starts[0]["position"], "SHORT");
    ASSERT_EQ(starts[0]["legs"].size(), 2u);
    EXPECT_EQ(starts[0]["legs"][0]["symbol"], "A");
    EXPECT_EQ(starts[0]["legs"][0]["side"], "SELL");
    EXPECT_TRUE(events("cycle_end").empty()) << "nothing resolves before the fills";

    fillFrom(0);
    auto fills = events("fill");
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0]["basket"], "tb");
    EXPECT_EQ(fills[0]["position_key"], "tb_A");
    EXPECT_FALSE(fills[0]["exec_id"].get<std::string>().empty());
    EXPECT_DOUBLE_EQ(fills[0]["position_after"].get<double>(), pm->getNetPosition("tb_A"));

    auto ends = events("cycle_end");
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_EQ(ends[0]["state"], "IN_POSITION");
    EXPECT_EQ(ends[0]["filled"], true);
}

TEST_F(JournalTest, ClosedJournalWritesNothing) {
    Journal::instance().close();
    std::filesystem::remove(file);
    closeDay(0.05);
    EXPECT_FALSE(std::filesystem::exists(file));
}

// state/zhistory/<id>.json: the dashboard's z chart before any close is journaled.
class ZHistoryTest : public StrategyHarness {};

TEST_F(ZHistoryTest, WarmUpWritesTheBotsOwnZForEveryFullWindowDay) {
    historyDir = (tmp / "zhistory").string();
    build();
    start();
    auto j = nlohmann::json::parse(std::ifstream(tmp / "zhistory" / "tb.json"));
    EXPECT_EQ(j["basket"], "tb");
    EXPECT_EQ(j["window"], kWindow);
    const auto& days = j["days"];
    ASSERT_EQ(days.size(), static_cast<size_t>(2 * kWindow - kWindow + 1)) << "40 days of history, window 20";
    EXPECT_EQ(days.back()[0].get<int64_t>(), day);
    EXPECT_DOUBLE_EQ(days.back()[1].get<double>(), strat->lastDailyZ().value());
}

TEST_F(ZHistoryTest, TradingIsTheSameWithOrWithoutIt) {
    auto run = [this](const std::string& dir) {
        if (strat && !stopped) stopStrategy();
        day = kLastHistoryDay;
        sessionOpen = false;
        historyDir = dir;
        build();
        start();
        std::vector<std::pair<double, PendingAction>> out;
        for (double s : {0.0, 0.05}) {
            closeDay(s);
            out.emplace_back(strat->lastDailyZ().value_or(99), strat->pendingAction());
        }
        return out;
    };
    auto plain = run("");
    auto with = run((tmp / "zhistory").string());
    EXPECT_EQ(plain, with);
    EXPECT_EQ(with.back().second, PendingAction::ENTER_SHORT);
}
