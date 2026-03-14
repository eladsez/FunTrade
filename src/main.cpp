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
#include <strategy/MarketEventPrinter.hpp>
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
    
    auto dataFeed = std::make_shared<AlpacaRtDataFeed>(apiKey, apiSecret);
    auto eventBus = std::make_shared<EventBus>(dataFeed);
    auto executionEngine = std::make_shared<AlpacaExecutionEngine>(eventBus, apiKey, apiSecret);


    auto portfolioValue = executionEngine->getPortfolioValue();
    double initPv = 100000.0;
    if (portfolioValue) {
        initPv = *portfolioValue;
        LOG_INFO("[LIVE] Current Portfolio Value: ${}", initPv);
    }

    auto eth_printer = std::make_shared<MarketEventPrinter>(eventBus, "ETH/USD");
    auto btc_printer = std::make_shared<MarketEventPrinter>(eventBus, "BTC/USD");

    eth_printer->start();
    btc_printer->start();

    std::this_thread::sleep_for(std::chrono::seconds(5));

    eth_printer->stop();
    std::this_thread::sleep_for(std::chrono::seconds(60));
    eth_printer->start();    

    LOG_INFO("[LIVE] Bot is running. Press Ctrl+C to stop.");
    
    // Wait infinitely until a signal is received
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    btc_printer->stop();
    eth_printer->stop();

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