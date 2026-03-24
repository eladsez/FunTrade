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
#include <infra/IbkrInfra.hpp>
#include <data/AlpacaRtDataFeed.hpp>
#include <data/CsvDataFeed.hpp>
#include <data/IbkrDataFeed.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <execution/MockExecutionEngine.hpp>
#include <execution/OrderManager.hpp>
#include <execution/IbkrExecutionEngine.hpp>
#include <risk/PortfolioManager.hpp>
#include <strategy/MarketEventPrinter.hpp>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <strategy/StrategyOrchestrator.hpp>

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
int runAlpacaLive(const std::string& apiKey, const std::string& apiSecret) {
    LOG_INFO("--- Starting LIVE Alpaca Paper Trading Mode ---");
    
    auto dataFeed = std::make_shared<AlpacaRtDataFeed>(apiKey, apiSecret);
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(apiKey, apiSecret);

    auto eventBus = std::make_shared<EventBus>(dataFeed, executionEngine);
    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(eventBus, executionEngine);

    double percentPerStrategy = 0.05; // Representing 5% of available cash

    // Initialize the Orchestrator
    LOG_INFO("[LIVE] Booting up Strategy Orchestrator...");
    StrategyOrchestrator orchestrator(
        eventBus, 
        orderManager, 
        portfolioManager, 
        "config/basket_configs", 
        percentPerStrategy
    );

    // Load and start all strategies
    orchestrator.loadStrategies();
    orchestrator.startAll();

    LOG_INFO("[LIVE] Bot is running with {} strategies. Press Ctrl+C to stop.", orchestrator.getStrategyCount());
    
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("[LIVE] Shutdown signal received. Stopping strategies...");
    orchestrator.stopAll();
    LOG_INFO("[LIVE] Shutdown complete.");
    return 0;
}

// ============================================================================
// BACKTESTING MODE
// ============================================================================
int runBacktestMode(const std::string& csvTickPath, const std::string& csvHistorPath, const std::string& configPath) {
    LOG_INFO("--- Starting HISTORICAL BACKTEST Mode ---");
    LOG_INFO("[Backtest] Data File: {}", csvTickPath);
    LOG_INFO("[Backtest] Config File: {}", configPath);

    // 1. Initialize Mocks
    auto dataFeed = std::make_shared<CsvDataFeed>(csvTickPath, csvHistorPath);
    auto eventBus = std::make_shared<EventBus>(dataFeed, nullptr);
    auto executionEngine = std::make_shared<MockExecutionEngine>(eventBus);

    // 2. Initialize Managers
    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(eventBus, executionEngine);

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

    double realized = portfolioManager->getTotalRealizedPnL();
    double unrealized = portfolioManager->getTotalUnrealizedPnL();
    int total_trades = portfolioManager->getTotalTrades();
    LOG_INFO("==================================================");
    LOG_INFO("              BACKTEST RESULTS                    ");
    LOG_INFO("==================================================");
    LOG_INFO("Total Fills:    {}", total_trades);
    LOG_INFO("Realized PnL:   ${:.2f}", realized);
    LOG_INFO("Unrealized PnL: ${:.2f}", unrealized);
    LOG_INFO("Total Net PnL:  ${:.2f}", (realized + unrealized));
    LOG_INFO("==================================================");
    
    LOG_INFO("[Backtest] Backtest complete.");
    return 0;
}

int runLiveIbkr() {
    // Initialize logging (assuming your Logger has a static init method or works out of the box)

    auto infra = std::make_shared<IbkrInfra>("host.docker.internal", 4002, 1); 

    auto execEngine = std::make_shared<IbkrExecutionEngine>(infra); 
    auto dataFeed = std::make_shared<IbkrDataFeed>(infra); 

    auto eventBus = std::make_shared<EventBus>(dataFeed, execEngine); 

    // 4. Connect the plumbing
    if (!infra->connectToTws()) {
        LOG_ERROR("[LIVE] Failed to connect to IB Gateway. Is it running and configured?\n");
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    execEngine->connectToInfra();
    dataFeed->connectToInfra();

    auto orderManager = std::make_shared<OrderManager>(eventBus, execEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(eventBus, execEngine);

    double percentPerStrategy = 0.05; // Representing 5% of available cash

    // Initialize the Orchestrator
    LOG_INFO("[LIVE] Booting up Strategy Orchestrator...");
    StrategyOrchestrator orchestrator(
        eventBus, 
        orderManager, 
        portfolioManager, 
        "config/basket_configs", 
        percentPerStrategy
    );

    // Load and start all strategies
    orchestrator.loadStrategies();
    orchestrator.startAll();

    LOG_INFO("[LIVE] Bot is running and printing Press Ctrl+C to stop.");
    
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("[LIVE] Shutting down...");

    infra->disconnectFromTws();
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

    std::string configPath = "config/basket_configs/amzn_cmg_expe.json";
    std::string csvTickPath = "data/testing_ticks.csv";
    std::string csvHistorPath = "";

    const char* apiKey = std::getenv("APCA_API_KEY_ID");
    const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

    // Simple CLI Argument Routing
    if (argc > 1 && std::string(argv[1]) == "--backtest") {
        if (argc > 2) {
            csvTickPath = argv[2];
        }
        if (argc > 3) {
            csvHistorPath = argv[3];
        }

        if (argc > 4) {
            configPath = argv[4];
        }

        return runBacktestMode(csvTickPath, csvHistorPath, configPath);
    } 
    else if (argc > 1 && std::string(argv[1]) == "--alpaca"){
        if (!apiKey || !apiSecret) {
            LOG_ERROR("\nERROR: Live mode requires Alpaca credentials.\n"
                      "To run a backtest instead, use: ./FunTrade --backtest\n");
            return 1;
        }
        return runAlpacaLive(apiKey, apiSecret);
    }
    else{
        runLiveIbkr();
    }
}