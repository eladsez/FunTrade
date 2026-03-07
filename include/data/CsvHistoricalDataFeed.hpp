#pragma once

#include <data/IDataFeed.hpp>
#include <core/IEventBus.hpp>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <fstream>
#include <vector>
#include <mutex>

namespace bot {

class CsvHistoricalDataFeed : public IDataFeed {
public:
    CsvHistoricalDataFeed(std::shared_ptr<IEventBus> eventBus, const std::string& csvFilePath);
    ~CsvHistoricalDataFeed() override;

    // IDataFeed Implementation
    bool connect() override;
    void subscribeToSymbol(const std::string& symbol) override;
    void disconnect() override;

    // Controls for backtesting
    void startPlayback(bool realTimeSimulation = false);
    void stopPlayback();

private:
    void playbackLoop(bool realTimeSimulation);
    MarketTick parseLine(const std::string& line);

    std::shared_ptr<IEventBus> eventBus_;
    std::string csvFilePath_;
    std::ifstream fileStream_;
    
    std::mutex symbolsMutex_;
    std::vector<std::string> subscribedSymbols_;

    std::atomic<bool> isPlaying_{false};
    std::thread playbackThread_;
};

} // namespace bot
