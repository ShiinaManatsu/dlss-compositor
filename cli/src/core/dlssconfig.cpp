#include "core/dlssconfig.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace dlss {
namespace {

using json = nlohmann::json;

constexpr float kJitterClamp = 0.499f;
constexpr float kJitterWarnThreshold = 0.5f;

LoadError makeError(LoadErrorCode code, std::string message, std::string path) {
    return LoadError{code, std::move(message), std::move(path)};
}

bool isFourDigitFrameKey(std::string_view value) {
    return value.size() == 4U && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool isPositiveInteger(const json& value) {
    return value.is_number_integer() && value.get<int>() > 0;
}

bool isNumberMatrix4x4(const json& value) {
    if (!value.is_array() || value.size() != 4U) {
        return false;
    }

    for (const auto& row : value) {
        if (!row.is_array() || row.size() != 4U) {
            return false;
        }
        for (const auto& cell : row) {
            if (!cell.is_number()) {
                return false;
            }
        }
    }

    return true;
}

bool hasObjectMember(const json& value, std::string_view key) {
    return value.is_object() && value.contains(key) && !value.at(key).is_null();
}

std::expected<std::reference_wrapper<const json>, LoadError> requireMember(
    const json& object,
    std::string_view key,
    std::string_view basePath,
    std::string_view expectedDescription) {
    if (!hasObjectMember(object, key)) {
        return std::unexpected(makeError(
            LoadErrorCode::SchemaFail,
            std::string("Missing or null ") + std::string(expectedDescription),
            std::string(basePath) + "/" + std::string(key)));
    }

    return std::cref(object.at(key));
}

std::expected<Resolution, LoadError> parseResolution(const json& object, std::string_view key) {
    const auto member = requireMember(object, key, "", "resolution object");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& resolution = member->get();
    const std::string basePath = std::string("/") + std::string(key);
    if (!resolution.is_object()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "Resolution must be an object", basePath));
    }

    if (!hasObjectMember(resolution, "width") || !isPositiveInteger(resolution.at("width"))) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "Resolution width must be a positive integer", basePath + "/width"));
    }

    if (!hasObjectMember(resolution, "height") || !isPositiveInteger(resolution.at("height"))) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "Resolution height must be a positive integer", basePath + "/height"));
    }

    return Resolution{
        static_cast<std::uint32_t>(resolution.at("width").get<int>()),
        static_cast<std::uint32_t>(resolution.at("height").get<int>())};
}

std::expected<QualityMode, LoadError> parseQualityMode(const json& root) {
    const auto member = requireMember(root, "quality_mode", "", "quality_mode");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& value = member->get();
    if (!value.is_string()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "quality_mode must be a string", "/quality_mode"));
    }

    const auto qualityMode = value.get<std::string>();
    if (qualityMode == "dlaa") {
        return QualityMode::DLAA;
    }
    if (qualityMode == "ultra_quality") {
        return QualityMode::UltraQuality;
    }
    if (qualityMode == "quality") {
        return QualityMode::Quality;
    }
    if (qualityMode == "balanced") {
        return QualityMode::Balanced;
    }
    if (qualityMode == "performance") {
        return QualityMode::Performance;
    }
    if (qualityMode == "ultra_performance") {
        return QualityMode::UltraPerformance;
    }

    return std::unexpected(makeError(LoadErrorCode::SchemaFail, "quality_mode has unsupported value", "/quality_mode"));
}

std::expected<std::string, LoadError> parseString(const json& root, std::string_view key) {
    const auto member = requireMember(root, key, "", "string field");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& value = member->get();
    if (!value.is_string()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, std::string(key) + " must be a string", std::string("/") + std::string(key)));
    }

    return value.get<std::string>();
}

std::expected<std::string, LoadError> parseOptionalPathString(
    const json& root,
    std::string_view key,
    const std::filesystem::path& baseDir) {
    if (!root.contains(key) || root.at(key).is_null()) {
        return std::string{};
    }

    const json& value = root.at(key);
    if (!value.is_string()) {
        return std::unexpected(makeError(
            LoadErrorCode::SchemaFail,
            std::string(key) + " must be a string",
            std::string("/") + std::string(key)));
    }

    const auto raw = value.get<std::string>();
    if (raw.empty()) {
        return std::string{};
    }

    std::filesystem::path path(raw);
    if (path.is_relative()) {
        path = baseDir / path;
    }
    return path.lexically_normal().string();
}

std::expected<bool, LoadError> parseBool(const json& root, std::string_view key) {
    const auto member = requireMember(root, key, "", "boolean field");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& value = member->get();
    if (!value.is_boolean()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, std::string(key) + " must be a boolean", std::string("/") + std::string(key)));
    }

    return value.get<bool>();
}

std::expected<int, LoadError> parsePositiveInt(const json& root, std::string_view key) {
    const auto member = requireMember(root, key, "", "positive integer field");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& value = member->get();
    if (!isPositiveInteger(value)) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, std::string(key) + " must be a positive integer", std::string("/") + std::string(key)));
    }

    return value.get<int>();
}

std::expected<float, LoadError> parseNumberField(const json& object, std::string_view key, std::string_view basePath) {
    const auto member = requireMember(object, key, basePath, "numeric field");
    if (!member.has_value()) {
        return std::unexpected(member.error());
    }

    const json& value = member->get();
    if (!value.is_number()) {
        return std::unexpected(makeError(
            LoadErrorCode::SchemaFail,
            std::string(key) + " must be numeric",
            std::string(basePath) + "/" + std::string(key)));
    }

    return value.get<float>();
}

float clampJitter(float value, std::string_view frameKey, std::string_view componentName) {
    if (std::fabs(value) > kJitterWarnThreshold) {
        const float clamped = std::clamp(value, -kJitterClamp, kJitterClamp);
        LOG_WARN("Clamped {} for frame {} from {} to {}", componentName, frameKey, value, clamped);
        return clamped;
    }

    return std::clamp(value, -kJitterClamp, kJitterClamp);
}

std::expected<CameraData, LoadError> parseFrame(const std::string& frameKey, const json& frame) {
    const std::string basePath = std::string("/frames/") + frameKey;
    if (!frame.is_object()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "Frame entry must be an object", basePath));
    }

    if (!hasObjectMember(frame, "matrix_world") || !isNumberMatrix4x4(frame.at("matrix_world"))) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "matrix_world must be a 4x4 numeric array", basePath + "/matrix_world"));
    }
    if (!hasObjectMember(frame, "projection") || !isNumberMatrix4x4(frame.at("projection"))) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "projection must be a 4x4 numeric array", basePath + "/projection"));
    }

    auto fov = parseNumberField(frame, "fov", basePath);
    if (!fov.has_value()) {
        return std::unexpected(fov.error());
    }
    auto aspectRatio = parseNumberField(frame, "aspect_ratio", basePath);
    if (!aspectRatio.has_value()) {
        return std::unexpected(aspectRatio.error());
    }
    auto nearClip = parseNumberField(frame, "near_clip", basePath);
    if (!nearClip.has_value()) {
        return std::unexpected(nearClip.error());
    }
    auto farClip = parseNumberField(frame, "far_clip", basePath);
    if (!farClip.has_value()) {
        return std::unexpected(farClip.error());
    }
    auto jitterX = parseNumberField(frame, "jitter_x", basePath);
    if (!jitterX.has_value()) {
        return std::unexpected(jitterX.error());
    }
    auto jitterY = parseNumberField(frame, "jitter_y", basePath);
    if (!jitterY.has_value()) {
        return std::unexpected(jitterY.error());
    }

    CameraData camera{};
    for (std::size_t row = 0; row < 4U; ++row) {
        for (std::size_t column = 0; column < 4U; ++column) {
            camera.matrix_world[row][column] = frame.at("matrix_world")[row][column].get<float>();
            camera.projection[row][column] = frame.at("projection")[row][column].get<float>();
        }
    }
    camera.fov = fov.value();
    camera.aspect_ratio = aspectRatio.value();
    camera.near_clip = nearClip.value();
    camera.far_clip = farClip.value();
    camera.jitter = JitterPair{
        clampJitter(jitterX.value(), frameKey, "jitter_x"),
        clampJitter(jitterY.value(), frameKey, "jitter_y")};

    return camera;
}

std::expected<std::map<std::string, CameraData>, LoadError> parseFrames(const json& root) {
    std::map<std::string, CameraData> frames;
    if (!root.contains("frames") || root.at("frames").is_null()) {
        return frames;
    }

    const json& jsonFrames = root.at("frames");
    if (!jsonFrames.is_object()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "frames must be an object", "/frames"));
    }

    for (auto it = jsonFrames.begin(); it != jsonFrames.end(); ++it) {
        const std::string frameKey = it.key();
        if (!isFourDigitFrameKey(frameKey)) {
            return std::unexpected(makeError(LoadErrorCode::FrameKeyMismatch, "Frame keys must match ^\\d{4}$", std::string("/frames/") + frameKey));
        }

        auto parsedFrame = parseFrame(frameKey, it.value());
        if (!parsedFrame.has_value()) {
            return std::unexpected(parsedFrame.error());
        }
        frames.emplace(frameKey, parsedFrame.value());
    }

    return frames;
}

} // namespace

std::expected<DlssConfig, LoadError> DlssConfigLoader::Load(std::filesystem::path filePath) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || ec) {
        return std::unexpected(makeError(LoadErrorCode::FileNotFound, "Config file was not found", filePath.string()));
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected(makeError(LoadErrorCode::FileNotFound, "Config file could not be opened", filePath.string()));
    }

    json root;
    try {
        input >> root;
    } catch (const json::parse_error& error) {
        return std::unexpected(makeError(LoadErrorCode::BadJson, error.what(), ""));
    }

    if (!root.is_object()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "Root JSON value must be an object", ""));
    }

    const auto versionMember = requireMember(root, "version", "", "version field");
    if (!versionMember.has_value()) {
        return std::unexpected(versionMember.error());
    }
    const json& versionValue = versionMember->get();
    if (!versionValue.is_number_integer()) {
        return std::unexpected(makeError(LoadErrorCode::SchemaFail, "version must be an integer", "/version"));
    }

    const int version = versionValue.get<int>();
    if (version != 1) {
        return std::unexpected(makeError(LoadErrorCode::VersionMismatch, "version must equal 1", "/version"));
    }

    auto outputResolution = parseResolution(root, "output_resolution");
    if (!outputResolution.has_value()) {
        return std::unexpected(outputResolution.error());
    }
    auto renderResolution = parseResolution(root, "render_resolution");
    if (!renderResolution.has_value()) {
        return std::unexpected(renderResolution.error());
    }
    auto qualityMode = parseQualityMode(root);
    if (!qualityMode.has_value()) {
        return std::unexpected(qualityMode.error());
    }
    auto displayDevice = parseString(root, "display_device");
    if (!displayDevice.has_value()) {
        return std::unexpected(displayDevice.error());
    }
    auto viewTransform = parseString(root, "view_transform");
    if (!viewTransform.has_value()) {
        return std::unexpected(viewTransform.error());
    }
    auto isHdr = parseBool(root, "is_hdr");
    if (!isHdr.has_value()) {
        return std::unexpected(isHdr.error());
    }
    auto fps = parsePositiveInt(root, "fps");
    if (!fps.has_value()) {
        return std::unexpected(fps.error());
    }
    auto filenamePattern = parseString(root, "filename_pattern");
    if (!filenamePattern.has_value()) {
        return std::unexpected(filenamePattern.error());
    }
    auto jitterPhases = parsePositiveInt(root, "jitter_phases");
    if (!jitterPhases.has_value()) {
        return std::unexpected(jitterPhases.error());
    }
    const auto configDir = std::filesystem::absolute(filePath).parent_path();
    auto tonemapLut = parseOptionalPathString(root, "tonemap_lut", configDir);
    if (!tonemapLut.has_value()) {
        return std::unexpected(tonemapLut.error());
    }
    auto inverseLut = parseOptionalPathString(root, "inverse_lut", configDir);
    if (!inverseLut.has_value()) {
        return std::unexpected(inverseLut.error());
    }
    auto frames = parseFrames(root);
    if (!frames.has_value()) {
        return std::unexpected(frames.error());
    }

    if (renderResolution->width > outputResolution->width || renderResolution->height > outputResolution->height) {
        return std::unexpected(makeError(
            LoadErrorCode::ResolutionInconsistent,
            "render_resolution must not exceed output_resolution",
            "/render_resolution"));
    }

    DlssConfig config{};
    config.version = version;
    config.output_resolution = outputResolution.value();
    config.render_resolution = renderResolution.value();
    config.quality = qualityMode.value();
    config.display_device = displayDevice.value();
    config.view_transform = viewTransform.value();
    config.is_hdr = isHdr.value();
    config.fps = fps.value();
    config.filename_pattern = filenamePattern.value();
    config.jitter_phases = jitterPhases.value();
    config.tonemap_lut = tonemapLut.value();
    config.inverse_lut = inverseLut.value();
    config.frames = std::move(frames.value());

    return config;
}

} // namespace dlss
