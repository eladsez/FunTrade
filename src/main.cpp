#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utils/magic_enum.hpp>
#include <risk/RiskManager.hpp>
#include <utils/TimeUtils.hpp>

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
    LOG_INFO("[Backtest] Warm-up File: {}", csvHistorPath.empty() ? "(none)" : csvHistorPath);
    LOG_INFO("[Backtest] Config File: {}", configPath);

    // Synchronous dispatch: every event (and the mock exchange's fills) is fully processed
    // before the next CSV row, so results are deterministic and comparable to compute.py.
    auto dataFeed = std::make_shared<CsvDataFeed>(csvTickPath, csvHistorPath);
    auto eventBus = std::make_shared<EventBus>(dataFeed, nullptr, DispatchMode::Synchronous);
    auto executionEngine = std::make_shared<MockExecutionEngine>(eventBus);

    auto orderManager = std::make_shared<OrderManager>(eventBus, executionEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(eventBus, executionEngine);

    double capitalAllocation = 5000.0;  // same as compute.py's max_capital
    auto strategy = std::make_shared<BasketMeanReversionStrategy>(
        eventBus, orderManager, portfolioManager, configPath, capitalAllocation
    );

    LOG_INFO("[Backtest] Booting up Strategy...");
    strategy->start();

    LOG_INFO("[Backtest] Pumping historical data...");
    dataFeed->startReplay(false); 
    while (dataFeed->isPlaying() && !g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("[Backtest] End of data reached. Stopping strategy...");
    dataFeed->stopReplay();
    strategy->stop();

    double realized = portfolioManager->getTotalRealizedPnL();
    double unrealized = portfolioManager->getTotalUnrealizedPnL();
    LOG_INFO("==================================================");
    LOG_INFO("              BACKTEST RESULTS                    ");
    LOG_INFO("==================================================");
    LOG_INFO("Final state:    {} / {}", std::string(magic_enum::enum_name(strategy->masterState())),
             std::string(magic_enum::enum_name(strategy->position())));
    LOG_INFO("Executions:     {}", portfolioManager->getTotalTrades());
    LOG_INFO("Realized PnL:   ${:.2f}", realized);
    LOG_INFO("Unrealized PnL: ${:.2f}", unrealized);
    LOG_INFO("Total Net PnL:  ${:.2f}", (realized + unrealized));
    LOG_INFO("==================================================");
    return 0;
}

// Risk limits from config/risk.json, scaled to the account's equity.
static RiskLimits loadRiskLimits(const std::string& path, double equity, double allocationPerBasket) {
    RiskLimits limits;
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream f(path); f.is_open()) f >> j;
    else LOG_WARN("[Risk] {} not found; using defaults.", path);

    limits.max_gross_exposure_usd = j.value("max_gross_exposure", 1.0) * equity;
    limits.max_symbol_net_usd = j.value("max_symbol_net", 0.10) * equity;
    limits.max_basket_loss_usd = j.value("max_basket_loss", 0.15) * allocationPerBasket * equity;
    limits.kill_switch_file = j.value("kill_switch_file", "state/KILL");
    limits.flatten_file = j.value("flatten_file", "state/FLATTEN");
    LOG_INFO("[Risk] Equity ${:.0f}: gross cap ${:.0f}, per-symbol net cap ${:.0f}, per-basket loss limit ${:.0f}. "
             "Kill switch: {}  Flatten: {}", equity, limits.max_gross_exposure_usd, limits.max_symbol_net_usd,
             limits.max_basket_loss_usd, limits.kill_switch_file, limits.flatten_file);
    return limits;
}

int runLiveIbkr() {
    const char* envHost = std::getenv("IBKR_HOST");
    const char* envPort = std::getenv("IBKR_PORT");
    const char* envClient = std::getenv("IBKR_CLIENT_ID");
    std::string host = envHost ? envHost : "127.0.0.1";
    int port = envPort ? std::stoi(envPort) : 4002;   // 4002 = IB Gateway paper
    int clientId = envClient ? std::stoi(envClient) : 1;

    auto infra = std::make_shared<IbkrInfra>(host, port, clientId);
    auto execEngine = std::make_shared<IbkrExecutionEngine>(infra); 
    auto dataFeed = std::make_shared<IbkrDataFeed>(infra); 
    auto eventBus = std::make_shared<EventBus>(dataFeed, execEngine); 

    // Books first, so the executions replayed while connecting are booked (deduped against
    // the persisted state) before anything trades.
    auto orderManager = std::make_shared<OrderManager>(eventBus, execEngine);
    auto portfolioManager = std::make_shared<PortfolioManager>(eventBus, execEngine, "state/positions.json");

    // Blocks until account, positions, orders and executions are synced.
    if (!execEngine->connectToInfra()) {
        LOG_ERROR("[LIVE] No complete account state from TWS. Exiting without trading.");
        return 1;
    }
    dataFeed->connectToInfra();
    eventBus->flush();              // replayed executions are now booked
    orderManager->syncWithExchange();
    portfolioManager->resync();     // reconciliation report; mismatching baskets halt on start

    const double allocationPerBasket = [] {
        nlohmann::json j = nlohmann::json::object();
        if (std::ifstream f("config/risk.json"); f.is_open()) f >> j;
        return j.value("allocation_per_basket", 0.05);
    }();
    // A non-USD account reports ExchangeRate[USD] only while it holds USD; until then take
    // the rate from IBKR's FX bars.
    auto base = execEngine->getBaseCurrency();
    if (!execEngine->getPortfolioValue() && base && *base != "USD") {
        auto now = std::chrono::system_clock::now();
        auto bars = dataFeed->fetchHistoricalBars("USD." + *base, utils::formatTimePoint(now - std::chrono::hours(24 * 7)),
                                                  utils::formatTimePoint(now), "1Day");
        if (!bars.empty()) {
            execEngine->setUsdRateFallback(bars.back().close);
            LOG_INFO("[LIVE] No USD balance; using USD.{} = {:.4f} from IBKR FX data.", *base, bars.back().close);
        }
    }
    auto equity = execEngine->getPortfolioValue();
    if (!equity) {
        LOG_ERROR("[LIVE] No account value (NetLiquidation/ExchangeRate) from TWS. Refusing to trade.");
        return 1;
    }

    StrategyOptions options;
    options.state_dir = "state/baskets";
    options.rebuild_window_daily = true;
    options.risk = std::make_shared<RiskManager>(portfolioManager,
        loadRiskLimits("config/risk.json", *equity, allocationPerBasket));

    LOG_INFO("[LIVE] Booting up Strategy Orchestrator...");
    StrategyOrchestrator orchestrator(
        eventBus, orderManager, portfolioManager, "config/basket_configs", allocationPerBasket, options);
    orchestrator.loadStrategies();
    orchestrator.startAll();

    LOG_INFO("[LIVE] Running {} baskets. Ctrl+C stops (positions persist; nothing is flattened).",
             orchestrator.getStrategyCount());
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    orchestrator.stopAll();
    LOG_INFO("[LIVE] Shutting down...");
    return 0;
}

// ============================================================================
// MAIN ROUTER
// ============================================================================
int main(int argc, char* argv[]) {

    bool debugMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--debug") {
            debugMode = true;
            break;
        }
    }

    bool backtest = argc > 1 && std::string(argv[1]) == "--backtest";
    bot::Logger::init(debugMode, backtest ? "" : "logs/funtrade.log");
    setupSignalHandlers();

    LOG_INFO("==================================================");
    LOG_INFO("          Algorithmic Trading Bot                 ");
    LOG_INFO("==================================================");
    if (debugMode) {
        LOG_DEBUG("Debug mode is ENABLED.");
    }

    std::string configPath = "config/basket_configs/amzn_cmg_ebay.json";
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
        return runLiveIbkr();
    }
}