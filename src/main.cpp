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
#include <data/CsvHistoricalDataFeed.hpp>
#include <data/AlpacaDataFeed.hpp>
#include <execution/DummyExecution.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <strategy/OBIMomentum.hpp>
#include <risk/PortfolioManager.hpp>
#include <execution/OrderManager.hpp>

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

int runLiveMode(const std::string& apiKey, const std::string& apiSecret, int windowSize = 20, double stdDevMultiplier = 2.0, double riskPercentage = 1.0, int emaWindowSize = 10) {
    LOG_INFO("--- Starting LIVE Alpaca Paper Trading Mode ---");
    std::string baseUrl = "https://paper-api.alpaca.markets";
    
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(*eventBus, apiKey, apiSecret, baseUrl);
    auto dataFeed = std::make_shared<AlpacaDataFeed>(eventBus, apiKey, apiSecret);
    auto portfolioManager = std::make_shared<PortfolioManager>(*eventBus);
    auto orderManager = std::make_shared<OrderManager>(*eventBus, *executionEngine);

    if (!executionEngine->authenticate()) {
        LOG_ERROR("Live Execution Authentication Failed.");
        return 1;
    }

    // Sync state with exchange
    portfolioManager->syncWithExchange(*executionEngine);
    orderManager->syncWithExchange();

    if (!dataFeed->connect()) {
        LOG_ERROR("Live DataFeed Connection Failed.");
        return 1;
    }
    
    // Allow time for WS handshake and authentication to complete
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    auto portfolioValue = executionEngine->getPortfolioValue();
    double initPv = 100000.0;
    if (portfolioValue) {
        initPv = *portfolioValue;
        LOG_INFO("[LIVE] Current Portfolio Value: ${}", initPv);
    }

    std::string symbol = "BTC/USD";
    LOG_INFO("[LIVE] Subscribing to {} market data stream...", symbol);
    
    // Use the dynamic parameters passed via command line
    auto strategy = std::make_shared<OBIMomentum>(eventBus, executionEngine, portfolioManager, initPv, symbol, windowSize, stdDevMultiplier, riskPercentage, emaWindowSize);

    
    // PortfolioManager should also get tick updates to track unrealized PnL
    eventBus->subscribeToTick(symbol, [portfolioManager](const MarketTick& tick) {
        portfolioManager->onTick(tick);
    });

    strategy->start();
    dataFeed->subscribeToSymbol(symbol);

    LOG_INFO("[LIVE] Bot is running. Press Ctrl+C to stop.");
    
    // Wait infinitely until a signal is received
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("[LIVE] Shutting down live session...");
    strategy->stop();
    dataFeed->disconnect();
    LOG_INFO("[LIVE] Shutdown complete.");
    return 0;
}

int runBacktestMode() {
    LOG_INFO("--- Starting Backtest Playback Mode ---");
    
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<DummyExecution>(*eventBus);
    auto dataFeed = std::make_shared<CsvHistoricalDataFeed>(eventBus, "data/obi_sample.csv");
    auto portfolioManager = std::make_shared<PortfolioManager>(*eventBus);
    auto strategy = std::make_shared<OBIMomentum>(eventBus, executionEngine, portfolioManager, 100000.0);

    executionEngine->authenticate();
    
    if (!dataFeed->connect()) {
        return 1;
    }
    
    std::string symbol = "BTC/USD";
    eventBus->subscribeToTick(symbol, [portfolioManager](const MarketTick& tick) {
        portfolioManager->onTick(tick);
    });

    strategy->start();
    dataFeed->subscribeToSymbol(symbol);
    
    dataFeed->startPlayback(true);
    
    LOG_INFO("Backtest running. Press Ctrl+C to abort early.");
    
    // Wait for shutdown signal OR a brief timeout (in reality backtests wait for EOF)
    // Since our Csv feed is threaded, we should ideally wait for it to finish.
    // For now, we just wait until the user stops it or 10 seconds pass.
    for (int i = 0; i < 100 && !g_shutdown_requested.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    strategy->stop();
    dataFeed->disconnect();

    LOG_INFO("Backtest completed.");
    return 0;
}

int main(int argc, char* argv[]) {
    // Initialize the global spdlog logger
    bot::Logger::init();

    setupSignalHandlers();

    LOG_INFO("==================================================");
    LOG_INFO("          Algorithmic Trading Bot                 ");
    LOG_INFO("==================================================");

    bool liveMode = false;
    int windowSize = 100;
    double stdDevMultiplier = 1.5;
    double riskPercentage = 1.0;
    int emaWindowSize = 500;
    

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--live") {
            liveMode = true;
        } else if (arg == "--window" && i + 1 < argc) {
            windowSize = std::stoi(argv[++i]);
        } else if (arg == "--stddev" && i + 1 < argc) {
            stdDevMultiplier = std::stod(argv[++i]);
        } else if (arg == "--risk" && i + 1 < argc) {
            riskPercentage = std::stod(argv[++i]);
        } else if (arg == "--ema" && i + 1 < argc) {
            emaWindowSize = std::stoi(argv[++i]);
        }
    }

    if (liveMode) {
        const char* apiKey = std::getenv("APCA_API_KEY_ID");
        const char* apiSecret = std::getenv("APCA_API_SECRET_KEY");

        if (!apiKey || !apiSecret) {
            LOG_ERROR("\nERROR: Live mode requires Alpaca credentials.\n"
                      "Please export them in your terminal before running:\n"
                      "  export APCA_API_KEY_ID=your_paper_key\n"
                      "  export APCA_API_SECRET_KEY=your_paper_secret\n");
            return 1;
        }
        return runLiveMode(apiKey, apiSecret, windowSize, stdDevMultiplier, riskPercentage, emaWindowSize);
    } else {
        return runBacktestMode();
    }
}
