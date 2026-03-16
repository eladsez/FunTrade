#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <core/Logger.hpp>
#include <core/EventBus.hpp>
#include <infra/AlpacaInfra.hpp>
#include <data/AlpacaRtDataFeed.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <risk/PortfolioManager.hpp>
#include <execution/OrderManager.hpp>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <iostream>

using namespace bot;

// Global coordination for graceful shutdown
std::atomic<bool> g_shutdown_requested{false};

void signalHandler(int /*signum*/) {
    // Async-signal-safe: only setting an atomic flag.
    g_shutdown_requested.store(true, std::memory_order_release);
}

void setupSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}

int runLiveMode(const std::string& apiKey, const std::string& apiSecret) {
    LOG_INFO("--- Starting LIVE Alpaca Paper Trading Mode ---");
    
    // 1. Initialize Core Infrastructure
    auto dataFeed = std::make_shared<AlpacaRtDataFeed>(apiKey, apiSecret);
    auto eventBus = std::make_shared<EventBus>(dataFeed);
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(eventBus, apiKey, apiSecret);

    // 2. Initialize the Execution & Risk Managers
    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    // Note: PortfolioManager takes references, so we dereference the shared_ptrs
    auto portfolioManager = std::make_shared<PortfolioManager>(*eventBus, *executionEngine);

    auto portfolioValue = executionEngine->getPortfolioValue();
    double initPv = 100000.0;
    if (portfolioValue) {
        initPv = *portfolioValue;
        LOG_INFO("[LIVE] Current Portfolio Value: ${}", initPv);
    }

    // 3. Initialize the Strategy
    std::string configPath = "scripts/robust_configs/amzn_cmg_expe.json";
    double capitalAllocation = 5000.0; 

    auto strategy = std::make_shared<BasketMeanReversionStrategy>(
        eventBus,
        orderManager,
        portfolioManager,
        configPath,
        capitalAllocation
    );

    // 4. Start the Strategy
    LOG_INFO("[LIVE] Booting up Basket Mean Reversion Strategy...");
    strategy->start();

    LOG_INFO("[LIVE] Bot is running. Press Ctrl+C to stop.");
    
    // Wait infinitely until a signal is received
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5. Graceful Shutdown
    LOG_INFO("[LIVE] Shutdown signal received. Stopping strategy.");
    strategy->stop();

    LOG_INFO("[LIVE] Shutdown complete.");
    return 0;
}

int main(int argc, char* argv[]) {
    // Initialize the global spdlog logger
    bot::Logger::init();

    setupSignalHandlers();

    LOG_INFO("==================================================");
    LOG_INFO("          Algorithmic Trading Bot                 ");
    LOG_INFO("==================================================");

    const char* apiKey = std::getenv("APCA_API_KEY_ID");
    const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

    if (!apiKey || !apiSecret) {
        LOG_ERROR("\nERROR: Live mode requires Alpaca credentials.\n"
                    "Please export them in your terminal before running:\n"
                    "  export APCA_API_KEY_ID=your_paper_key\n"
                    "  export APCA_API_SECRET_KEY=your_paper_secret\n");
        return 1;
    }
    return runLiveMode(apiKey, apiSecret);
}