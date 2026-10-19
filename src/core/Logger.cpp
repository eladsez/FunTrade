#include <core/Logger.hpp>
#include <vector>

namespace bot {

std::shared_ptr<spdlog::logger> Logger::logger_;

void Logger::init(bool debugMode, const std::string& logFile) {
    if (!logger_) {
        // Create console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        std::vector<spdlog::sink_ptr> sinks {console_sink};
        if (!logFile.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile, 10 * 1024 * 1024, 10));
        }
        
        logger_ = std::make_shared<spdlog::logger>("FunTrade", sinks.begin(), sinks.end());

        // Format: [YYYY-MM-DD HH:MM:SS.ms] [logger] [level] message
        logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
        
        // Set global log level
        if (debugMode) {
            logger_->set_level(spdlog::level::debug);
        } else {
            logger_->set_level(spdlog::level::info);
        }
        
        // Setup SPDLOG macros to use our logger level
        spdlog::set_default_logger(logger_);
        // Errors must reach disk even if the process dies right after.
        logger_->flush_on(spdlog::level::warn);
    }
}

std::shared_ptr<spdlog::logger>& Logger::getLogger() {
    if (!logger_) {
        init();
    }
    return logger_;
}

} // namespace bot
