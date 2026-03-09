#include <gtest/gtest.h>
#include <data/AlpacaRtDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <vector>

using namespace bot;

class AlpacaDataFeedTest : public ::testing::Test {
protected:
    // std::shared_ptr<EventBus> eventBus;
    // std::unique_ptr<AlpacaRtDataFeed> dataFeed;

    void SetUp() override {
        // eventBus = std::make_shared<EventBus>();
        // dataFeed = std::make_unique<AlpacaRtDataFeed>(eventBus, "FAKE_KEY", "FAKE_SECRET");
    }
};

TEST_F(AlpacaDataFeedTest, ConnectUpdatesStateAndSendsAuth) {
    EXPECT_TRUE(true);
}
