#include <gtest/gtest.h>
#include <core/EventBus.hpp>
#include <atomic>
#include <chrono>

using namespace bot;


class EventBusTest : public ::testing::Test {
protected:
    std::unique_ptr<EventBus> eventBus;

    void SetUp() override {
        eventBus = std::make_unique<EventBus>(nullptr, nullptr);
    }

    void TearDown() override {}
};