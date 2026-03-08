#include <core/Logger.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <vector>

namespace bot {

std::shared_ptr<spdlog::logger> Logger::logger_;

void Logger::init() {
    if (!logger_) {
        // Create console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        // Create a rotating file sink (max 5MB per file, max 3 rotated files)
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/funtrade.log", 1024 * 1024 * 5, 3);
        
        std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
        
        logger_ = std::make_shared<spdlog::logger>("FunTrade", sinks.begin(), sinks.end());

        // Format: [YYYY-MM-DD HH:MM:SS.ms] [logger] [level] message
        logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
        
        // Set global log level
        logger_->set_level(spdlog::level::debug);
        
        // Setup SPDLOG macros to use our logger level
        spdlog::set_default_logger(logger_);
    }
}

std::shared_ptr<spdlog::logger>& Logger::getLogger() {
    if (!logger_) {
        init();
    }
    return logger_;
}

} // namespace bot
