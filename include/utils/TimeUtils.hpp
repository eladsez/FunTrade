#pragma once
#include <chrono>
#include <string>
#include <iomanip>
#include <sstream>

namespace bot {
namespace utils {

// Helper function to generate RFC3339 / ISO8601 timestamps
inline std::string formatTimePoint(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val;

#if defined(_WIN32)
    gmtime_s(&tm_val, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm_val);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace utils
} // namespace bot