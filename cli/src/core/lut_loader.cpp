#include "core/lut_loader.h"

#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace dlss {
namespace {

LutLoadError makeError(LutLoadErrorCode code, std::string message, const std::filesystem::path& path) {
    return LutLoadError{code, std::move(message), path.string()};
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string_view> splitTokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    line = trim(line);
    while (!line.empty()) {
        const auto pos = line.find_first_of(" \t\r\n");
        if (pos == std::string_view::npos) {
            tokens.push_back(line);
            break;
        }
        tokens.push_back(line.substr(0, pos));
        line.remove_prefix(pos + 1);
        line = trim(line);
    }
    return tokens;
}

bool parseFloat(std::string_view token, float& out) {
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parseInt(std::string_view token, int& out) {
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

void parseDlssMetadata(std::string_view comment, Lut3D& lut) {
    comment = trim(comment);
    if (!comment.starts_with("DLSS_")) {
        return;
    }
    const auto tokens = splitTokens(comment);
    if (tokens.size() < 2U) {
        return;
    }
    if (tokens[0] == "DLSS_LUT_INPUT_ENCODING" && tokens[1] == "log2") {
        lut.input_log2 = true;
        return;
    }
    if (tokens[0] == "DLSS_LUT_LOG2_MIN") {
        float value = 0.0f;
        if (parseFloat(tokens[1], value)) {
            lut.log2_min = value;
        }
        return;
    }
    if (tokens[0] == "DLSS_LUT_LOG2_MAX") {
        float value = 0.0f;
        if (parseFloat(tokens[1], value)) {
            lut.log2_max = value;
        }
    }
}

std::expected<std::array<float, 3>, LutLoadError> parseVec3(
    const std::vector<std::string_view>& tokens,
    const std::filesystem::path& path,
    std::string_view fieldName) {
    if (tokens.size() != 4U) {
        return std::unexpected(makeError(
            LutLoadErrorCode::BadFormat,
            std::string(fieldName) + " must have exactly three values",
            path));
    }

    std::array<float, 3> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!parseFloat(tokens[i + 1U], values[i])) {
            return std::unexpected(makeError(
                LutLoadErrorCode::BadFormat,
                std::string(fieldName) + " contains a non-numeric value",
                path));
        }
    }
    return values;
}

} // namespace

std::expected<Lut3D, LutLoadError> LutLoader::LoadCube(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected(makeError(LutLoadErrorCode::FileNotFound, "LUT file could not be opened", filePath));
    }

    Lut3D lut{};
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        auto content = trim(line);
        const auto comment = content.find('#');
        if (comment != std::string_view::npos) {
            parseDlssMetadata(content.substr(comment + 1U), lut);
            content = trim(content.substr(0, comment));
        }
        if (content.empty()) {
            continue;
        }

        const auto tokens = splitTokens(content);
        if (tokens.empty()) {
            continue;
        }

        const auto keyword = tokens.front();
        if (keyword == "TITLE") {
            continue;
        }
        if (keyword == "LUT_1D_SIZE") {
            return std::unexpected(makeError(LutLoadErrorCode::Unsupported, "1D LUT .cube files are not supported", filePath));
        }
        if (keyword == "LUT_3D_SIZE") {
            if (tokens.size() != 2U || !parseInt(tokens[1], lut.size) || lut.size <= 1) {
                return std::unexpected(makeError(LutLoadErrorCode::BadFormat, "LUT_3D_SIZE must be an integer greater than 1", filePath));
            }
            continue;
        }
        if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            auto values = parseVec3(tokens, filePath, keyword);
            if (!values.has_value()) {
                return std::unexpected(values.error());
            }
            float* target = keyword == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max;
            for (std::size_t i = 0; i < values->size(); ++i) {
                target[i] = values->at(i);
            }
            continue;
        }

        if (tokens.size() != 3U) {
            return std::unexpected(makeError(
                LutLoadErrorCode::BadFormat,
                "Unexpected token count on LUT data line " + std::to_string(lineNumber),
                filePath));
        }

        for (const auto token : tokens) {
            float value = 0.0f;
            if (!parseFloat(token, value)) {
                return std::unexpected(makeError(
                    LutLoadErrorCode::BadFormat,
                    "Invalid numeric LUT value on line " + std::to_string(lineNumber),
                    filePath));
            }
            lut.rgb.push_back(value);
        }
    }

    if (lut.size <= 1) {
        return std::unexpected(makeError(LutLoadErrorCode::BadFormat, "Missing LUT_3D_SIZE", filePath));
    }

    const auto expectedFloats = static_cast<std::size_t>(lut.size) *
        static_cast<std::size_t>(lut.size) *
        static_cast<std::size_t>(lut.size) * 3U;
    if (lut.rgb.size() != expectedFloats) {
        return std::unexpected(makeError(
            LutLoadErrorCode::BadFormat,
            "LUT data count does not match LUT_3D_SIZE",
            filePath));
    }

    return lut;
}

} // namespace dlss
