#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dlss {

// JSON and media libraries exchange UTF-8. Windows filesystem paths use UTF-16.
inline std::filesystem::path pathFromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

inline std::string pathToUtf8(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace dlss
