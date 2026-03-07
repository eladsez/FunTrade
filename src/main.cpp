#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <core/Logger.hpp>
#include <core/EventBus.hpp>
#include <data/CsvHistoricalDataFeed.hpp>
#include <data/AlpacaDataFeed.hpp>
#include <execution/DummyExecution.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <strategy/OBIMomentum.hpp>

using namespace bot;

int runLiveMode(const std::string& apiKey, const std::string& apiSecret) {
    LOG_INFO("--- Starting LIVE Alpaca Paper Trading Mode ---");
    std::string baseUrl = "https://paper-api.alpaca.markets";
    
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(apiKey, apiSecret, baseUrl);
    auto dataFeed = std::make_shared<AlpacaDataFeed>(eventBus, apiKey, apiSecret);

    if (!executionEngine->authenticate()) {
        LOG_ERROR("Live Execution Authentication Failed.");
        return 1;
    }

    if (!dataFeed->connect()) {
        LOG_ERROR("Live DataFeed Connection Failed.");
        return 1;
    }
    
    // Allow time for WS handshake and authentication to complete
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    auto portfolioValue = executionEngine->getPortfolioValue();
    if (portfolioValue) {
        LOG_INFO("[LIVE] Current Portfolio Value: ${}", *portfolioValue);
    }

    std::string symbol = "BTC/USD";
    LOG_INFO("[LIVE] Subscribing to {} market data stream...", symbol);
    auto strategy = std::make_shared<OBIMomentum>(eventBus, executionEngine, symbol);
    strategy->start();
    dataFeed->subscribeToSymbol(symbol);

    LOG_INFO("[LIVE] Listening for 20 seconds of live market data before shutting down...");
    std::this_thread::sleep_for(std::chrono::seconds(20));

    LOG_INFO("[LIVE] Shutting down live session...");
    strategy->stop();
    dataFeed->disconnect();
    return 0;
}

int runBacktestMode() {
    LOG_INFO("--- Starting Backtest Playback Mode ---");
    
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<DummyExecution>();
    auto dataFeed = std::make_shared<CsvHistoricalDataFeed>(eventBus, "data/obi_sample.csv");
    auto strategy = std::make_shared<OBIMomentum>(eventBus, executionEngine);

    executionEngine->authenticate();
    
    if (!dataFeed->connect()) {
        return 1;
    }
    
    strategy->start();
    dataFeed->subscribeToSymbol("BTC/USD");
    
    dataFeed->startPlayback(true);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    strategy->stop();
    dataFeed->disconnect();

    LOG_INFO("Backtest successful.");
    return 0;
}

int main(int argc, char* argv[]) {
    // Initialize the global spdlog logger
    bot::Logger::init();

    LOG_INFO("Algorithmic Trading Bot PoC");

    bool liveMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--live") {
            liveMode = true;
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
        return runLiveMode(apiKey, apiSecret);
    } else {
        return runBacktestMode();
    }
}
