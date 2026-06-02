#include "core/log.h"

#include <cstdlib>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace dlss::log {
namespace {

constexpr auto kLoggerName = "dlss";
constexpr auto kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";
constexpr std::size_t kMaxFileSize = 5U * 1024U * 1024U;
constexpr std::size_t kMaxFiles = 3U;

spdlog::level::level_enum parseLevel(std::string_view level) {
    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "info") {
        return spdlog::level::info;
    }
    if (level == "warn") {
        return spdlog::level::warn;
    }
    if (level == "error") {
        return spdlog::level::err;
    }
    if (level == "off") {
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

std::string envLevel() {
    const char* value = std::getenv("DLSS_COMPOSITOR_LOG");
    if (value == nullptr || *value == '\0') {
        return "info";
    }

    std::string level = value;
    for (char& ch : level) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return level;
}

class Log {
public:
    static Log& instance() {
        static Log log;
        return log;
    }

    void init() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureInitializedLocked();
    }

    void setLevel(std::string_view level) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensureInitializedLocked();

        if (auto logger = spdlog::get(kLoggerName)) {
            logger->set_level(parseLevel(level));
        }
    }

    void log(Level level, std::string_view message) {
        init();

        auto logger = spdlog::get(kLoggerName);
        if (!logger) {
            return;
        }

        switch (level) {
        case Level::trace:
            logger->trace(message);
            break;
        case Level::debug:
            logger->debug(message);
            break;
        case Level::info:
            logger->info(message);
            break;
        case Level::warn:
            logger->warn(message);
            break;
        case Level::error:
            logger->error(message);
            break;
        }
    }

private:
    void ensureInitializedLocked() {
        if (spdlog::get(kLoggerName)) {
            return;
        }

        auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto rotatingFileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("dlss-compositor.log", kMaxFileSize, kMaxFiles);
        std::vector<spdlog::sink_ptr> sinks{stderrSink, rotatingFileSink};

        auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        logger->set_pattern(kPattern);
        logger->set_level(parseLevel(envLevel()));
        logger->flush_on(spdlog::level::trace);

        spdlog::register_logger(logger);
    }

    std::mutex mutex_;
};

}  // namespace

void init() {
    Log::instance().init();
}

void setLevel(const std::string& level) {
    Log::instance().setLevel(level);
}

namespace detail {

void log_message(Level level, std::string message) {
    Log::instance().log(level, message);
}

}  // namespace detail
}  // namespace dlss::log
