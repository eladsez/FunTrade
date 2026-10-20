#pragma once
#include <nlohmann/json.hpp>
#include <core/Logger.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace bot {

// Append-only event log for the dashboard: one JSON object per line ("t" = epoch ms,
// "type", then the event's fields). The bot never reads it back, so trading can't depend on
// it; a failed write only logs. Closed (the default: tests, backtests) means every record is
// a no-op.
//
// Event types: start, stop, close (each basket's daily decision), cycle_start, cycle_end,
// fill, entry_skipped, halt.
class Journal {
public:
    static Journal& instance() {
        static Journal j;
        return j;
    }

    void open(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        out_.close();
        out_.clear();
        out_.open(path, std::ios::app);
        if (!out_) LOG_ERROR("[Journal] Cannot open {}", path.string());
    }

    void close() {
        std::lock_guard<std::mutex> lock(mtx_);
        out_.close();
    }

    void record(const std::string& type, nlohmann::json fields = nlohmann::json::object()) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!out_.is_open()) return;
        nlohmann::json line = {{"t", nowMs()}, {"type", type}};
        line.update(fields);
        out_ << line.dump() << '\n';
        out_.flush();
        if (!out_) {
            LOG_ERROR("[Journal] Write failed for a {} event", type);
            out_.clear();
        }
    }

private:
    Journal() = default;

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::mutex mtx_;
    std::ofstream out_;
};

} // namespace bot
