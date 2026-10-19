#pragma once
#include <gtest/gtest.h>
#include <strategy/BasketMeanReversionStrategy.hpp>
#include <execution/OrderManager.hpp>
#include <risk/PortfolioManager.hpp>
#include <risk/RiskManager.hpp>
#include "SyncEventBus.hpp"
#include "FakeExecutionEngine.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <unistd.h>

namespace bot::test {

// Drives a real BasketMeanReversionStrategy (with real OrderManager, PortfolioManager and
// optional RiskManager) through trading days on a synchronous bus and a scripted engine.
// Basket "tb" = A - B (weights 1, -1), window 20, entry_z 1.5, exit_z 0.
//
//   closeDay(s)   publishes the daily bars of a day whose spread is s (A = 100*e^s, B = 100).
//                 It's the current day if openSession() started one, else the next day.
//   openSession() first ticks of the next trading day (pending actions run here).
//   fillFrom(i)   fills every order placed from index i at the latest prices.
class StrategyHarness : public ::testing::Test {
protected:
    static constexpr int kWindow = 20;
    static constexpr int64_t kLastHistoryDay = 20000;  // 2024-10-04, safely in the past

    std::filesystem::path tmp;
    std::shared_ptr<SyncEventBus> bus;
    std::shared_ptr<FakeExecutionEngine> engine;
    std::shared_ptr<OrderManager> om;
    std::shared_ptr<PortfolioManager> pm;
    std::shared_ptr<RiskManager> risk;
    std::shared_ptr<BasketMeanReversionStrategy> strat;
    std::function<void(RiskLimits&)> riskConfig;
    bool persist = false;
    bool stopped = false;
    bool sessionOpen = false;
    int64_t day = kLastHistoryDay;
    std::map<std::string, double> last{{"A", 100.0}, {"B", 100.0}};

    StrategyHarness() {
        static int n = 0;
        tmp = std::filesystem::temp_directory_path() /
              ("funtrade_test_" + std::to_string(::getpid()) + "_" + std::to_string(++n));
        std::filesystem::create_directories(tmp);
        std::ofstream(tmp / "tb.json") << R"({"basket_id":"tb","tickers":["A","B"],"weights":{"A":1.0,"B":-1.0},
            "trading_params":{"window_size":20,"entry_z":1.5,"exit_z":0.0}})";
    }

    void TearDown() override {
        if (strat && !stopped) stopStrategy();
        std::filesystem::remove_all(tmp);
    }

    static uint64_t ms(int64_t d) { return static_cast<uint64_t>(d) * 86'400'000ULL; }
    static double spreadAt(int i) { return 0.005 * std::sin(i * 0.7); }

    // (Re)creates the components. Keeping `engine` keeps the fake broker's positions and
    // orders, which is how restart tests simulate the account surviving a bot restart.
    void build(bool keepEngine = false) {
        bus = std::make_shared<SyncEventBus>();
        if (!keepEngine || !engine) engine = std::make_shared<FakeExecutionEngine>();
        engine->bus = bus.get();
        engine->onPlace = nullptr;
        engine->onCancel = nullptr;
        om = std::make_shared<OrderManager>(bus, engine);
        pm = std::make_shared<PortfolioManager>(bus, engine, persist ? (tmp / "positions.json").string() : "");
        StrategyOptions opts;
        if (persist) opts.state_dir = (tmp / "baskets").string();
        if (riskConfig) {
            RiskLimits limits;
            riskConfig(limits);
            risk = std::make_shared<RiskManager>(pm, limits);
            opts.risk = risk;
        }
        strat = std::make_shared<BasketMeanReversionStrategy>(bus, om, pm, (tmp / "tb.json").string(), 100000.0, opts);
        stopped = false;

        // History ends at the last CLOSED day: a restart mid-session never sees today's bar
        // (the strategy drops bars dated today because they are still forming).
        int64_t lastClosed = sessionOpen ? day - 1 : day;
        std::vector<HistoricalBar> a, b;
        for (int i = 0; i < 2 * kWindow; ++i) {
            int64_t d = lastClosed - (2 * kWindow - 1) + i;
            double s = (i == 2 * kWindow - 1) ? 0.0 : spreadAt(i);  // last day at the mean: no signal at start
            if (day != kLastHistoryDay) s = 0.0;                     // restarts: flat history
            a.push_back(HistoricalBar{"A", ms(d), 0, 0, 0, 100.0 * std::exp(s), 1e6, 0, 0});
            b.push_back(HistoricalBar{"B", ms(d), 0, 0, 0, 100.0, 1e6, 0, 0});
        }
        bus->setHistory("A", a);
        bus->setHistory("B", b);
    }

    void start() { strat->start(); }

    // Fresh components with a risk configuration (same day, fresh fake broker).
    void rebuild(std::function<void(RiskLimits&)> cfg) {
        if (strat && !stopped) stopStrategy();
        riskConfig = std::move(cfg);
        build();
        start();
    }

    void stopStrategy() {
        autoCancel();
        strat->stop();
        stopped = true;
    }

    void closeDay(double spread) {
        if (!sessionOpen) day++;
        sessionOpen = false;
        last["A"] = 100.0 * std::exp(spread);
        last["B"] = 100.0;
        bus->publishMarketEvent(BarUpdate{"A", 0, 0, 0, last["A"], 1e6, ms(day)});
        bus->publishMarketEvent(BarUpdate{"B", 0, 0, 0, last["B"], 1e6, ms(day)});
    }

    void openSession() {
        day++;
        sessionOpen = true;
        bus->publishMarketEvent(MidPointTick{"A", last["A"], ms(day) + 34'200'000});
        bus->publishMarketEvent(MidPointTick{"B", last["B"], ms(day) + 34'200'000});
    }

    void tick(double spread) {
        bus->publishMarketEvent(MidPointTick{"A", 100.0 * std::exp(spread), ms(day) + 40'000'000});
        bus->publishMarketEvent(MidPointTick{"B", 100.0, ms(day) + 40'000'000});
    }

    void fillFrom(size_t i) {
        engine->fillAll([this](const std::string& s) { return last.at(s); }, i);
    }

    const OrderRequest& placedFor(const std::string& symbol) const {
        for (auto it = engine->placed.rbegin(); it != engine->placed.rend(); ++it)
            if (it->symbol == symbol) return *it;
        throw std::runtime_error("no order for " + symbol);
    }

    void autoCancel() {
        engine->onCancel = [this](const std::string& id) {
            if (auto* r = engine->findPlaced(id)) engine->emit(FakeExecutionEngine::report(*r, OrderStatus::CANCELED, 0, 0, 0));
        };
    }
};

} // namespace bot::test
