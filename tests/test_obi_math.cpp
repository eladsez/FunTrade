#include <gtest/gtest.h>
#include <strategy/OBIMomentum.hpp>
#include <core/EventBus.hpp>
#include <execution/DummyExecution.hpp>
#include <memory>

using namespace bot;

class OBIMathTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::shared_ptr<DummyExecution> executionEngine;
    std::unique_ptr<OBIMomentum> strategy;

    void SetUp() override {
        eventBus = std::make_shared<EventBus>();
        executionEngine = std::make_shared<DummyExecution>(*eventBus);
        // windowSize = 5, stdDevMultiplier = 1.0, risk = 1.0, emaWindow = 5
        strategy = std::make_unique<OBIMomentum>(eventBus, executionEngine, "BTC/USD", 5, 1.0, 1.0, 5);
        strategy->start();
    }

    void TearDown() override {
        strategy->stop();
    }
};

TEST_F(OBIMathTest, BasicInitialization) {
    EXPECT_TRUE(true);
}
