#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>

#include <core/Logger.hpp>
#include <core/EventBus.hpp>
#include <infra/AlpacaInfra.hpp>
#include <data/AlpacaRtDataFeed.hpp>
#include <data/CsvDataFeed.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <execution/MockExecutionEngine.hpp>
#include <execution/OrderManager.hpp>
#include <risk/PortfolioManager.hpp>
#include <strategy/BasketMeanReversionStrategy.hpp>

using namespace bot;

// Global coordination for graceful shutdown
std::atomic<bool> g_shutdown_requested{false};

void signalHandler(int /*signum*/) {
    g_shutdown_requested.store(true, std::memory_order_release);
}

void setupSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}

// ============================================================================
// LIVE TRADING MODE
// ============================================================================
int runLiveMode(const std::string& apiKey, const std::string& apiSecret, const std::string& configPath) {
    LOG_INFO("--- Starting LIVE Alpaca Paper Trading Mode ---");
    
    auto dataFeed = std::make_shared<AlpacaRtDataFeed>(apiKey, apiSecret);
    auto eventBus = std::make_shared<EventBus>(dataFeed);
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(eventBus, apiKey, apiSecret);

    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(*eventBus, *executionEngine);

    auto portfolioValue = executionEngine->getPortfolioValue();
    double initPv = 100000.0;
    if (portfolioValue) {
        initPv = *portfolioValue;
        LOG_INFO("[LIVE] Current Portfolio Value: ${}", initPv);
    }

    double capitalAllocation = 5000.0; 
    auto strategy = std::make_shared<BasketMeanReversionStrategy>(
        eventBus, orderManager, portfolioManager, configPath, capitalAllocation
    );

    LOG_INFO("[LIVE] Booting up Basket Mean Reversion Strategy...");
    strategy->start();

    LOG_INFO("[LIVE] Bot is running. Press Ctrl+C to stop.");
    
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("[LIVE] Shutdown signal received. Stopping strategy");
    strategy->stop();
    LOG_INFO("[LIVE] Shutdown complete.");
    return 0;
}

// ============================================================================
// BACKTESTING MODE
// ============================================================================
int runBacktestMode(const std::string& csvFilePath, const std::string& configPath) {
    LOG_INFO("--- Starting HISTORICAL BACKTEST Mode ---");
    LOG_INFO("[Backtest] Data File: {}", csvFilePath);
    LOG_INFO("[Backtest] Config File: {}", configPath);

    // 1. Initialize Mocks
    auto dataFeed = std::make_shared<CsvDataFeed>(csvFilePath);
    auto eventBus = std::make_shared<EventBus>(dataFeed);
    auto executionEngine = std::make_shared<MockExecutionEngine>(eventBus);

    // 2. Initialize Managers
    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(*eventBus, *executionEngine);

    // 3. Initialize Strategy
    double capitalAllocation = 5000.0; 
    auto strategy = std::make_shared<BasketMeanReversionStrategy>(
        eventBus, orderManager, portfolioManager, configPath, capitalAllocation
    );

    // 4. Start Strategy
    LOG_INFO("[Backtest] Booting up Strategy...");
    strategy->start();

    // 5. Start the CSV Data Pump (false = max speed, no real-time sleep)
    LOG_INFO("[Backtest] Pumping historical data...");
    dataFeed->startReplay(false); 

    // Wait until the CSV finishes reading or the user hits Ctrl+C
    while (dataFeed->isPlaying() && !g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 6. Graceful Shutdown
    LOG_INFO("[Backtest] End of data reached. Stopping strategy...");
    dataFeed->stopReplay();
    strategy->stop();
    
    LOG_INFO("[Backtest] Backtest complete.");
    return 0;
}

// ============================================================================
// MAIN ROUTER
// ============================================================================
int main(int argc, char* argv[]) {
    bot::Logger::init();
    setupSignalHandlers();

    LOG_INFO("==================================================");
    LOG_INFO("          Algorithmic Trading Bot                 ");
    LOG_INFO("==================================================");

    std::string configPath = "scripts/robust_configs/amzn_cmg_expe.json";

    // Simple CLI Argument Routing
    if (argc > 1 && std::string(argv[1]) == "--backtest") {
        std::string csvPath = "data/testing_ticks.csv";
        if (argc > 2) {
            csvPath = argv[2]; // Allow passing a custom CSV path
        }
        return runBacktestMode(csvPath, configPath);
    } 
    else {
        const char* apiKey = std::getenv("APCA_API_KEY_ID");
        const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

        if (!apiKey || !apiSecret) {
            LOG_ERROR("\nERROR: Live mode requires Alpaca credentials.\n"
                      "To run a backtest instead, use: ./FunTrade --backtest\n");
            return 1;
        }
        return runLiveMode(apiKey, apiSecret, configPath);
    }
}