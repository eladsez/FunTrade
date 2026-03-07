#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <core/EventBus.hpp>
#include <data/CsvHistoricalDataFeed.hpp>
#include <data/AlpacaDataFeed.hpp>
#include <execution/DummyExecution.hpp>
#include <execution/AlpacaExecutionEngine.hpp>
#include <strategy/OBIMomentum.hpp>

using namespace bot;

int runLiveMode(const std::string& apiKey, const std::string& apiSecret) {
    std::cout << "--- Starting LIVE Alpaca Paper Trading Mode ---" << std::endl;
    std::string baseUrl = "https://paper-api.alpaca.markets";
    
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(apiKey, apiSecret, baseUrl);
    auto dataFeed = std::make_shared<AlpacaDataFeed>(eventBus, apiKey, apiSecret);

    if (!executionEngine->authenticate()) {
        std::cerr << "Live Execution Authentication Failed." << std::endl;
        return 1;
    }

    if (!dataFeed->connect()) {
        std::cerr << "Live DataFeed Connection Failed." << std::endl;
        return 1;
    }
    
    // Allow time for WS handshake and authentication to complete
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // We will test the REST API by fetching portfolio value
    auto portfolioValue = executionEngine->getPortfolioValue();
    if (portfolioValue) {
        std::cout << "[LIVE] Current Portfolio Value: $" << *portfolioValue << std::endl;
    }

    // Subscribe to a highly liquid symbol for the test
    std::string symbol = "BTC/USD";
    std::cout << "[LIVE] Subscribing to " << symbol << " market data stream..." << std::endl;
    auto strategy = std::make_shared<OBIMomentum>(eventBus, executionEngine, symbol);
    strategy->start();
    dataFeed->subscribeToSymbol(symbol);

    std::cout << "[LIVE] Listening for 20 seconds of live market data before shutting down..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(20));

    // Cleanup
    std::cout << "[LIVE] Shutting down live session..." << std::endl;
    strategy->stop();
    dataFeed->disconnect();
    return 0;
}

int runBacktestMode() {
    std::cout << "--- Starting Backtest Playback Mode ---" << std::endl;
    
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

    std::cout << "Backtest successful." << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    std::cout << "Algorithmic Trading Bot PoC" << std::endl;

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
            std::cerr << "\nERROR: Live mode requires Alpaca credentials.\n"
                      << "Please export them in your terminal before running:\n"
                      << "  export APCA_API_KEY_ID=your_paper_key\n"
                      << "  export APCA_API_SECRET_KEY=your_paper_secret\n"
                      << std::endl;
            return 1;
        }
        return runLiveMode(apiKey, apiSecret);
    } else {
        return runBacktestMode();
    }
}
