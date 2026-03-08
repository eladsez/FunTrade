#include <gtest/gtest.h>
#include <strategy/OBIMomentum.hpp>
#include <core/EventBus.hpp>
#include <execution/DummyExecution.hpp>
#include <risk/PortfolioManager.hpp>
#include <memory>

using namespace bot;

class OBIMathTest : public ::testing::Test {
protected:
    std::shared_ptr<EventBus> eventBus;
    std::shared_ptr<DummyExecution> executionEngine;
    std::shared_ptr<PortfolioManager> portfolioManager;
    std::unique_ptr<OBIMomentum> strategy;

    void SetUp() override {
        eventBus = std::make_shared<EventBus>();
        executionEngine = std::make_shared<DummyExecution>(*eventBus);
        portfolioManager = std::make_shared<PortfolioManager>(*eventBus);
        // windowSize = 5, stdDevMultiplier = 1.0, risk = 1.0, emaWindow = 5
        strategy = std::make_unique<OBIMomentum>(eventBus, executionEngine, portfolioManager, 100000.0, "BTC/USD", 5, 1.0, 1.0, 5);
        strategy->start();
    }

    void TearDown() override {
        strategy->stop();
    }
};

TEST_F(OBIMathTest, BasicInitialization) {
    EXPECT_TRUE(true);
}
