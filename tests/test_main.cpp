#include <gtest/gtest.h>
#include <core/Logger.hpp>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    // Keep test output readable; set FUNTRADE_TEST_LOGS=1 to see the bot's logs.
    bot::Logger::init(false);
    if (!std::getenv("FUNTRADE_TEST_LOGS")) bot::Logger::getLogger()->set_level(spdlog::level::off);
    return RUN_ALL_TESTS();
}
