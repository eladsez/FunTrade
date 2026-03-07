#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <core/EventBus.hpp>
#include <data/CsvHistoricalDataFeed.hpp>
#include <execution/DummyExecution.hpp>
#include <strategy/OBIMomentum.hpp>

int main() {
    std::cout << "Starting Trading Bot Backtester..." << std::endl;
    
    // 1. Initialize components
    auto eventBus = std::make_shared<EventBus>();
    auto executionEngine = std::make_shared<DummyExecution>();
    
    // Use the new CSV Historical Feed pointing to our sample data
    auto dataFeed = std::make_shared<bot::CsvHistoricalDataFeed>(eventBus, "data/obi_sample.csv");
    
    auto strategy = std::make_shared<bot::OBIMomentum>(eventBus, executionEngine);

    // 2. Wire them up & start
    executionEngine->authenticate();
    
    if (!dataFeed->connect()) {
        return 1;
    }
    
    strategy->start();
    
    // Strategy logic listens for BTC/USD
    dataFeed->subscribeToSymbol("BTC/USD");

    // 3. Start Historical Playback
    std::cout << "--- Starting Backtest Playback ---" << std::endl;
    
    // We'll use real-time simulation so we can see the logs clearly
    dataFeed->startPlayback(true);
    
    // Wait for the playback to finish (it's asynchronous)
    // In a real backtester, we'd join the thread or wait for an EOF event
    // Our sample has about 500ms of data, so 1 second is plenty
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 4. Cleanup
    strategy->stop();
    dataFeed->disconnect();

    std::cout << "Backtest successful. Exiting gracefully." << std::endl;
    return 0;
}
