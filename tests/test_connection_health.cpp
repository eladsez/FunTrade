#include <gtest/gtest.h>

#include <infra/ConnectionHealth.hpp>

using namespace bot;
using Level = ConnectionHealth::Level;

namespace {
constexpr int64_t kMin = 60'000;
constexpr int64_t T0 = 1'792'540'000'000;  // any epoch ms
}  // namespace

TEST(ConnectionHealth, StartsDisconnectedAndConnectsQuietly) {
    ConnectionHealth h;
    EXPECT_FALSE(h.snapshot().socket);
    EXPECT_FALSE(h.snapshot().ibkr);
    EXPECT_FALSE(h.socketUp(T0).has_value()) << "first connect: nothing to report";
    auto s = h.snapshot();
    EXPECT_TRUE(s.socket);
    EXPECT_TRUE(s.ibkr);
    EXPECT_FALSE(s.ibkr_down_since.has_value());
}

TEST(ConnectionHealth, Error1100MarksTheGatewayCutOffWhileTheSocketStaysUp) {
    ConnectionHealth h;
    h.socketUp(T0);
    auto n = h.ibkrDown(T0 + 1000);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->level, Level::Warn);
    auto s = h.snapshot();
    EXPECT_TRUE(s.socket);
    EXPECT_FALSE(s.ibkr);
    EXPECT_EQ(s.ibkr_down_since, T0 + 1000);

    auto again = h.ibkrDown(T0 + 2000);   // TWS repeats 1100 while it retries
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->level, Level::Debug);
    EXPECT_EQ(h.snapshot().ibkr_down_since, T0 + 1000) << "the outage keeps its start";
}

TEST(ConnectionHealth, ShortOutageStaysAWarningAndRecoveryIsInfo) {
    ConnectionHealth h;
    h.socketUp(T0);
    h.ibkrDown(T0);
    EXPECT_FALSE(h.tick(T0 + 4 * kMin).has_value()) << "within the grace period";
    auto n = h.ibkrUp(T0 + 4 * kMin + 30'000, false);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->level, Level::Info);
    EXPECT_NE(n->text.find("4 min"), std::string::npos) << n->text;
    EXPECT_NE(n->text.find("1102"), std::string::npos) << n->text;
    EXPECT_TRUE(h.snapshot().ibkr);
    EXPECT_FALSE(h.snapshot().ibkr_down_since.has_value());
    EXPECT_FALSE(h.tick(T0 + 60 * kMin).has_value());
}

TEST(ConnectionHealth, LongOutageEscalatesOnceThenReminds) {
    // 2026-10-21: the Gateway's re-login failed and it sent nothing more, so only the
    // periodic tick can notice.
    ConnectionHealth h;
    h.socketUp(T0);
    h.ibkrDown(T0);
    auto e = h.tick(T0 + 5 * kMin);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->level, Level::Error);
    EXPECT_NE(e->text.find("5 min"), std::string::npos) << e->text;
    EXPECT_FALSE(h.tick(T0 + 6 * kMin).has_value());
    EXPECT_FALSE(h.tick(T0 + 34 * kMin).has_value());
    auto r = h.tick(T0 + 35 * kMin);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->level, Level::Error);

    auto back = h.ibkrUp(T0 + 40 * kMin, true);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->level, Level::Warn) << "an outage that was an error ends as a warning";
    EXPECT_NE(back->text.find("1101"), std::string::npos);
}

TEST(ConnectionHealth, ANewGatewaySessionEndsTheOutage) {
    // The fix on 2026-10-21 was restarting ib-gateway: the socket drops, then reconnects to a
    // freshly logged-in Gateway, which sends no 1102.
    ConnectionHealth h;
    h.socketUp(T0);
    h.ibkrDown(T0);
    h.socketDown(T0 + 10 * kMin);
    auto s = h.snapshot();
    EXPECT_FALSE(s.socket);
    EXPECT_FALSE(s.ibkr) << "no IBKR link without the socket";
    EXPECT_FALSE(h.tick(T0 + 20 * kMin).has_value()) << "the IBKR outage isn't escalated while the socket is down";

    auto n = h.socketUp(T0 + 11 * kMin);
    ASSERT_TRUE(n.has_value());
    EXPECT_NE(n->text.find("new Gateway session"), std::string::npos);
    EXPECT_TRUE(h.snapshot().ibkr);
}

TEST(ConnectionHealth, DataLostWithoutAnOutageStillSaysSo) {
    ConnectionHealth h;
    h.socketUp(T0);
    auto n = h.ibkrUp(T0, true);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->level, Level::Info);
    EXPECT_FALSE(h.ibkrUp(T0, false).has_value());
}

TEST(ConnectionHealth, Durations) {
    EXPECT_EQ(ConnectionHealth::duration(46'000), "46 s");
    EXPECT_EQ(ConnectionHealth::duration(12 * kMin + 5000), "12 min");
    EXPECT_EQ(ConnectionHealth::duration(9 * 60 * kMin + 24 * kMin), "9 h 24 min");
    EXPECT_EQ(ConnectionHealth::duration(2 * 60 * kMin + 5 * kMin), "2 h 05 min");
}
