#pragma once

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#if defined(__GNUC__) && __GNUC__ >= 13
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Warray-bounds"
    #pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#if defined(__GNUC__) && __GNUC__ >= 13
    #pragma GCC diagnostic pop
#endif

#include <memory>

namespace bot {

class Logger {
public:
    // logFile: also write to this rotating file (10 MB x 10), for unattended runs.
    static void init(bool debugMode = false, const std::string& logFile = "");
    static std::shared_ptr<spdlog::logger>& getLogger();

private:
    static std::shared_ptr<spdlog::logger> logger_;
};

} // namespace bot

// Convenience macros for logging
#define LOG_INFO(...)  SPDLOG_LOGGER_INFO(::bot::Logger::getLogger(), __VA_ARGS__)
#define LOG_WARN(...)  SPDLOG_LOGGER_WARN(::bot::Logger::getLogger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(::bot::Logger::getLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::bot::Logger::getLogger(), __VA_ARGS__)
