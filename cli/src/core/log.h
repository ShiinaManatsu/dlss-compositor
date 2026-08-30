#pragma once

// Public header intentionally avoids spdlog includes.

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace dlss { namespace log {

enum class Level {
    trace,
    debug,
    info,
    warn,
    error,
};

void init(bool resetFile = false);
void setLevel(const std::string& level);

namespace detail {
void log_message(Level level, std::string message);
}

inline void impl_trace(std::string_view message) {
    detail::log_message(Level::trace, std::string(message));
}

template <typename... Args>
inline void impl_trace(std::string_view format, Args&&... args) {
    detail::log_message(Level::trace, fmt::vformat(format, fmt::make_format_args(args...)));
}

inline void impl_debug(std::string_view message) {
    detail::log_message(Level::debug, std::string(message));
}

template <typename... Args>
inline void impl_debug(std::string_view format, Args&&... args) {
    detail::log_message(Level::debug, fmt::vformat(format, fmt::make_format_args(args...)));
}

inline void impl_info(std::string_view message) {
    detail::log_message(Level::info, std::string(message));
}

template <typename... Args>
inline void impl_info(std::string_view format, Args&&... args) {
    detail::log_message(Level::info, fmt::vformat(format, fmt::make_format_args(args...)));
}

inline void impl_warn(std::string_view message) {
    detail::log_message(Level::warn, std::string(message));
}

template <typename... Args>
inline void impl_warn(std::string_view format, Args&&... args) {
    detail::log_message(Level::warn, fmt::vformat(format, fmt::make_format_args(args...)));
}

inline void impl_error(std::string_view message) {
    detail::log_message(Level::error, std::string(message));
}

template <typename... Args>
inline void impl_error(std::string_view format, Args&&... args) {
    detail::log_message(Level::error, fmt::vformat(format, fmt::make_format_args(args...)));
}

}}  // namespace dlss::log

#define LOG_TRACE(...) ::dlss::log::impl_trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::dlss::log::impl_debug(__VA_ARGS__)
#define LOG_INFO(...)  ::dlss::log::impl_info(__VA_ARGS__)
#define LOG_WARN(...)  ::dlss::log::impl_warn(__VA_ARGS__)
#define LOG_ERROR(...) ::dlss::log::impl_error(__VA_ARGS__)
