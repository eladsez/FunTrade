#pragma once

#include <data/IDataFeed.hpp>
#include <core/DomainModels.hpp>
#include <core/Logger.hpp>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

namespace bot {

class CsvDataFeed : public IDataFeed {
public:
    explicit CsvDataFeed(const std::string& tickCsvPath, const std::string& warmupCsvPath = "")
        :is_playing_(false), file_path_(tickCsvPath), warmup_file_path_(warmupCsvPath) {}

    ~CsvDataFeed() override {
        disconnectFromInfra();
    }

    bool connectToInfra() override {
        LOG_INFO("[CsvDataFeed] Connected to virtual CSV file: {}", file_path_);
        return true;
    }

    void disconnectFromInfra() override {
        stopReplay();
        LOG_INFO("[CsvDataFeed] Disconnected from CSV file.");
    }

    void subscribeToSymbol(const std::string& symbol) override {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        subscriptions_.insert(symbol);
        LOG_INFO("[CsvDataFeed] Subscribed to {}", symbol);
    }

    void unsubscribeFromSymbol(const std::string& symbol) override {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        subscriptions_.erase(symbol);
        LOG_INFO("[CsvDataFeed] Unsubscribed from {}", symbol);
    }

    std::vector<std::string> getActiveSubscriptions() const override {
        std::lock_guard<std::mutex> lock(sub_mtx_);
        return {subscriptions_.begin(), subscriptions_.end()};
    }

    std::vector<HistoricalBar> fetchHistoricalBars(
        const std::string& symbol, 
        const std::string&/* start */, 
        const std::string& /* end */, 
        const std::string& /* timeframe */) override {
        
        std::vector<HistoricalBar> bars;

        if (warmup_file_path_.empty()) {
            LOG_WARN("[CsvDataFeed] No warmup file provided. Returning empty history.");
            return bars;
        }

        std::ifstream file(warmup_file_path_);
        if (!file.is_open()) {
            LOG_ERROR("[CsvDataFeed] Failed to open warmup CSV: {}", warmup_file_path_);
            return bars;
        }

        std::string line;
        // Skip header
        std::getline(file, line);

        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string token;

            try {
                // Expected format from Python: timestamp_ms,symbol,close,volume
                std::getline(ss, token, ',');
                uint64_t ts = std::stoull(token);

                std::getline(ss, token, ',');
                std::string row_symbol = token;

                // Only parse and store bars for the requested symbol
                if (row_symbol == symbol) {
                    std::getline(ss, token, ',');
                    double close = std::stod(token);

                    std::getline(ss, token, ',');
                    double volume = std::stod(token);

                    HistoricalBar bar;
                    bar.timestamp_ms = ts;
                    bar.symbol = row_symbol;
                    bar.close = close;
                    bar.volume = volume;
                    
                    bars.push_back(bar);
                }
            } catch (const std::exception& e) {
                LOG_ERROR("[CsvDataFeed] Error parsing warmup line: {} | {}", line, e.what());
            }
        }

        LOG_INFO("[CsvDataFeed] Fetched {} historical bars for {}", bars.size(), symbol);
        return bars;
    }

    void setMarketTickCallback(std::function<void(const MarketEvent&)> callback) override {
        tick_callback_ = std::move(callback);
    }

    // Custom Backtesting API
    void startReplay(bool runInRealTime = false) {
        if (is_playing_) return;
        is_playing_ = true;

        replay_thread_ = std::thread([this, runInRealTime]() {
            std::ifstream file(file_path_);
            if (!file.is_open()) {
                LOG_ERROR("[CsvDataFeed] Failed to open CSV: {}", file_path_);
                is_playing_ = false;
                return;
            }

            std::string line;
            // Skip the header row
            std::getline(file, line);

            uint64_t last_ts = 0;

            LOG_INFO("[CsvDataFeed] Starting CSV replay...");

            while (is_playing_ && std::getline(file, line)) {
                if (line.empty()) continue;

                
                // for debug
                // std::this_thread::sleep_for(std::chrono::milliseconds(5));


                std::stringstream ss(line);
                std::string token;

                try {
                    // 1. timestamp_ms
                    std::getline(ss, token, ',');
                    uint64_t ts = std::stoull(token);

                    // 2. symbol
                    std::getline(ss, token, ',');
                    std::string symbol = token;

                    // Filter out symbols we haven't subscribed to early
                    {
                        std::lock_guard<std::mutex> lock(sub_mtx_);
                        if (!subscriptions_.empty() && subscriptions_.find(symbol) == subscriptions_.end()) {
                            continue;
                        }
                    }

                    // 3. side or event_type
                    std::getline(ss, token, ',');
                    std::string side_str = token;

                    // 4. price
                    std::getline(ss, token, ',');
                    double price = std::stod(token);

                    // 5. volume
                    std::getline(ss, token, ',');
                    double volume = std::stod(token);

                    // 6. trade_id (clear the stream)
                    std::getline(ss, token, ',');

                    // Optional: Sleep to simulate realistic arrival times
                    if (runInRealTime && last_ts > 0 && ts > last_ts) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(ts - last_ts));
                    }
                    last_ts = ts;

                    // --- ROUTE TRADES VS BARS ---
                    if (side_str == "bar") {
                        BarUpdate bar;
                        bar.timestamp_ms = ts;
                        bar.symbol = symbol;
                        bar.close = price;
                        bar.volume = volume;
                        
                        // Fire the Daily Close to the Strategy!
                        if (tick_callback_) tick_callback_(bar);
                    } else {
                        TradeTick tick;
                        tick.timestamp_ms = ts;
                        tick.symbol = symbol;
                        tick.price = price;
                        tick.volume = volume;
                        tick.taker_side = (side_str == "buy" || side_str == "BUY") ? Side::BUY : Side::SELL;
                        
                        // Fire the normal tick to the Strategy
                        if (tick_callback_) tick_callback_(tick);
                    }

                } catch (const std::exception& e) {
                    LOG_ERROR("[CsvDataFeed] Error parsing line: {} | Exception: {}", line, e.what());
                }
            }

            LOG_INFO("[CsvDataFeed] CSV Replay Complete.");
            is_playing_ = false;
        });
    }

    void stopReplay() {
        is_playing_ = false;
        if (replay_thread_.joinable()) {
            replay_thread_.join();
        }
    }

    bool isPlaying() const { return is_playing_.load(); }

private:
    std::set<std::string> subscriptions_;
    mutable std::mutex sub_mtx_;
    std::function<void(const MarketEvent&)> tick_callback_;
    
    std::atomic<bool> is_playing_;
    std::thread replay_thread_;

    std::string file_path_;
    std::string warmup_file_path_;
};

} // namespace bot