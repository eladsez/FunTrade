#include <data/CsvHistoricalDataFeed.hpp>
#include <core/Logger.hpp>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace bot {

CsvHistoricalDataFeed::CsvHistoricalDataFeed(std::shared_ptr<IEventBus> eventBus, const std::string& csvFilePath)
    : eventBus_(std::move(eventBus)), csvFilePath_(csvFilePath) {}

CsvHistoricalDataFeed::~CsvHistoricalDataFeed() {
    stopPlayback();
}

bool CsvHistoricalDataFeed::connect() {
    fileStream_.open(csvFilePath_);
    if (!fileStream_.is_open()) {
        LOG_ERROR("[CsvHistoricalDataFeed] Failed to open file: {}", csvFilePath_);
        return false;
    }
    LOG_INFO("[CsvHistoricalDataFeed] Successfully opened {} for backtesting.", csvFilePath_);
    return true;
}

void CsvHistoricalDataFeed::subscribeToSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(symbolsMutex_);
    if (std::find(subscribedSymbols_.begin(), subscribedSymbols_.end(), symbol) == subscribedSymbols_.end()) {
        subscribedSymbols_.push_back(symbol);
        LOG_INFO("[CsvHistoricalDataFeed] Filtering playback for {}", symbol);
    }
}

void CsvHistoricalDataFeed::disconnect() {
    stopPlayback();
    if (fileStream_.is_open()) {
        fileStream_.close();
        LOG_INFO("[CsvHistoricalDataFeed] Closed file.");
    }
}

void CsvHistoricalDataFeed::startPlayback(bool realTimeSimulation) {
    if (!fileStream_.is_open()) {
        LOG_ERROR("[CsvHistoricalDataFeed] Cannot start playback. File is not open.");
        return;
    }

    if (isPlaying_.exchange(true)) return;

    // Rewind to beginning and skip the CSV header
    fileStream_.clear();
    fileStream_.seekg(0);
    std::string header;
    std::getline(fileStream_, header);

    playbackThread_ = std::thread(&CsvHistoricalDataFeed::playbackLoop, this, realTimeSimulation);
    LOG_INFO("[CsvHistoricalDataFeed] Started playback {}.", (realTimeSimulation ? "(Real-Time Delay)" : "(As Fast As Possible)"));
}

void CsvHistoricalDataFeed::stopPlayback() {
    isPlaying_ = false;

    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    LOG_INFO("[CsvHistoricalDataFeed] Stopped playback.");
}

void CsvHistoricalDataFeed::playbackLoop(bool realTimeSimulation) {
    std::string line;
    uint64_t lastTimestamp = 0;

    while (isPlaying_ && std::getline(fileStream_, line)) {
        if (line.empty()) continue;

        try {
            MarketTick tick = parseLine(line);

            bool isSubscribed = false;
            {
                std::lock_guard<std::mutex> lock(symbolsMutex_);
                isSubscribed = subscribedSymbols_.empty() || 
                               std::find(subscribedSymbols_.begin(), subscribedSymbols_.end(), tick.symbol) != subscribedSymbols_.end();
            }

            if (isSubscribed && eventBus_) {
                // If realTimeSimulation is true, wait exactly as long as the historical delay between ticks
                if (realTimeSimulation && lastTimestamp != 0 && tick.timestamp_ms > lastTimestamp) {
                    auto delay = std::chrono::milliseconds(tick.timestamp_ms - lastTimestamp);
                    std::this_thread::sleep_for(delay);
                }
                
                eventBus_->publishTick(tick);
                lastTimestamp = tick.timestamp_ms;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[CsvHistoricalDataFeed] Error parsing line: {} | {}", line, e.what());
        }
    }
    isPlaying_ = false;
    LOG_INFO("[CsvHistoricalDataFeed] Playback finished. Reached EOF.");
}

MarketTick CsvHistoricalDataFeed::parseLine(const std::string& line) {
    // Expected strict format: timestamp_ms,symbol,side,price,volume,trade_id
    std::stringstream ss(line);
    std::string token;
    MarketTick tick;

    // 1. timestamp_ms
    if (!std::getline(ss, token, ',')) throw std::runtime_error("Missing timestamp");
    tick.timestamp_ms = std::stoull(token);

    // 2. symbol
    if (!std::getline(ss, tick.symbol, ',')) throw std::runtime_error("Missing symbol");

    // 3. side (skip for now since our DomainModel doesn't track historical sides natively yet)
    if (!std::getline(ss, token, ',')) throw std::runtime_error("Missing side");

    // 4. price
    if (!std::getline(ss, token, ',')) throw std::runtime_error("Missing price");
    tick.price = std::stod(token);

    // 5. volume
    if (!std::getline(ss, token, ',')) throw std::runtime_error("Missing volume");
    tick.volume = std::stod(token);

    // 6. trade_id (skip)
    if (std::getline(ss, token, ',')) {
        // Optional L2 data: bid_price, bid_size, ask_price, ask_size
        if (std::getline(ss, token, ',')) tick.bid_price = std::stod(token);
        if (std::getline(ss, token, ',')) tick.bid_size = std::stod(token);
        if (std::getline(ss, token, ',')) tick.ask_price = std::stod(token);
        if (std::getline(ss, token, ',')) tick.ask_size = std::stod(token);
    }

    return tick;
}

} // namespace bot