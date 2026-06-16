#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/dlssconfig.h"
#include "core/types.h"

namespace {

std::filesystem::path writeTempJson(const std::string& fileName, const nlohmann::json& value) {
    const auto path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2);
    output.close();
    return path;
}

void requireLoadError(const std::expected<dlss::DlssConfig, dlss::LoadError>& result, dlss::LoadErrorCode code) {
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == code);
}

} // namespace

TEST_CASE("Load valid config", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/sample-dlssconfig.json");
    REQUIRE(result.has_value());
    REQUIRE(result->version == 1);
    REQUIRE(result->fps == 30);
}

TEST_CASE("Load valid config maps metadata", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/sample-dlssconfig.json");
    REQUIRE(result.has_value());
    REQUIRE(result->output_resolution.width == 1920);
    REQUIRE(result->output_resolution.height == 1080);
    REQUIRE(result->render_resolution.width == 1280);
    REQUIRE(result->render_resolution.height == 720);
    REQUIRE(result->quality == dlss::QualityMode::Quality);
    REQUIRE(result->display_device == "sRGB");
    REQUIRE(result->view_transform == "Filmic");
    REQUIRE(result->filename_pattern == "frame_####.png");
    REQUIRE(result->jitter_phases == 18);
    REQUIRE(std::filesystem::path(result->tonemap_lut).filename() == "tonemap_srgb_filmic_33.cube");
    REQUIRE(std::filesystem::path(result->inverse_lut).filename() == "inverse_srgb_filmic_33.cube");
    REQUIRE(std::filesystem::path(result->tonemap_lut).is_absolute());
    REQUIRE(std::filesystem::path(result->inverse_lut).is_absolute());
}

TEST_CASE("Load valid config keeps LUT paths optional", "[dlssconfig]") {
    nlohmann::json root = {
        {"version", 1},
        {"output_resolution", {{"width", 1920}, {"height", 1080}}},
        {"render_resolution", {{"width", 1280}, {"height", 720}}},
        {"quality_mode", "quality"},
        {"display_device", "sRGB"},
        {"view_transform", "Filmic"},
        {"is_hdr", false},
        {"fps", 30},
        {"filename_pattern", "frame_####.png"},
        {"jitter_phases", 18}};

    const auto path = writeTempJson("dlssconfig-loader-no-lut.json", root);
    auto result = dlss::DlssConfigLoader::Load(path);

    REQUIRE(result.has_value());
    REQUIRE(result->tonemap_lut.empty());
    REQUIRE(result->inverse_lut.empty());
}

TEST_CASE("Load valid config maps frame payload", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/sample-dlssconfig.json");
    REQUIRE(result.has_value());
    REQUIRE(result->frames.size() == 5);

    const auto& frame = result->frames.at("0001");
    REQUIRE(frame.matrix_world[2][3] == Catch::Approx(5.0f));
    REQUIRE(frame.projection[2][3] == Catch::Approx(-0.2f));
    REQUIRE(frame.fov == Catch::Approx(0.6911f));
    REQUIRE(frame.aspect_ratio == Catch::Approx(1.7777778f));
    REQUIRE(frame.jitter.x == Catch::Approx(0.125f));
    REQUIRE(frame.jitter.y == Catch::Approx(-0.125f));
}

TEST_CASE("Jitter clamp edge tolerance is silent", "[dlssconfig]") {
    nlohmann::json root = {
        {"version", 1},
        {"output_resolution", {{"width", 1920}, {"height", 1080}}},
        {"render_resolution", {{"width", 1280}, {"height", 720}}},
        {"quality_mode", "quality"},
        {"display_device", "sRGB"},
        {"view_transform", "Filmic"},
        {"is_hdr", false},
        {"fps", 30},
        {"filename_pattern", "frame_####.png"},
        {"jitter_phases", 18},
        {"frames",
         {{"0001",
           {{"matrix_world", {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 1.0, 5.0}, {0.0, 0.0, 0.0, 1.0}}},
            {"projection", {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, -1.0, -0.2}, {0.0, 0.0, -1.0, 0.0}}},
            {"fov", 0.6911},
            {"aspect_ratio", 1.7777778},
            {"near_clip", 0.1},
            {"far_clip", 100.0},
            {"jitter_x", 0.5},
            {"jitter_y", -0.5}}}}}};

    const auto path = writeTempJson("dlssconfig-loader-edge.json", root);
    auto result = dlss::DlssConfigLoader::Load(path);

    REQUIRE(result.has_value());
    REQUIRE(result->frames.at("0001").jitter.x == Catch::Approx(0.499f));
    REQUIRE(result->frames.at("0001").jitter.y == Catch::Approx(-0.499f));
}

TEST_CASE("Jitter clamp - never reject", "[dlssconfig]") {
    nlohmann::json root = {
        {"version", 1},
        {"output_resolution", {{"width", 1920}, {"height", 1080}}},
        {"render_resolution", {{"width", 1280}, {"height", 720}}},
        {"quality_mode", "quality"},
        {"display_device", "sRGB"},
        {"view_transform", "Filmic"},
        {"is_hdr", false},
        {"fps", 30},
        {"filename_pattern", "frame_####.png"},
        {"jitter_phases", 18},
        {"frames",
         {{"0001",
           {{"matrix_world", {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, 1.0, 5.0}, {0.0, 0.0, 0.0, 1.0}}},
            {"projection", {{1.0, 0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0}, {0.0, 0.0, -1.0, -0.2}, {0.0, 0.0, -1.0, 0.0}}},
            {"fov", 0.6911},
            {"aspect_ratio", 1.7777778},
            {"near_clip", 0.1},
            {"far_clip", 100.0},
            {"jitter_x", -0.55},
            {"jitter_y", 1.0}}}}}};

    const auto path = writeTempJson("dlssconfig-loader-extreme.json", root);
    auto result = dlss::DlssConfigLoader::Load(path);

    REQUIRE(result.has_value());
    REQUIRE(result->frames.at("0001").jitter.x == Catch::Approx(-0.499f));
    REQUIRE(result->frames.at("0001").jitter.y == Catch::Approx(0.499f));
}

TEST_CASE("BadJson", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-json.json");
    requireLoadError(result, dlss::LoadErrorCode::BadJson);
}

TEST_CASE("VersionMismatch", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-version.json");
    requireLoadError(result, dlss::LoadErrorCode::VersionMismatch);
    REQUIRE(result.error().path == "/version");
}

TEST_CASE("SchemaFail missing fps", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-schema.json");
    requireLoadError(result, dlss::LoadErrorCode::SchemaFail);
    REQUIRE(result.error().path == "/fps");
}

TEST_CASE("ResolutionInconsistent", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-resolution.json");
    requireLoadError(result, dlss::LoadErrorCode::ResolutionInconsistent);
    REQUIRE(result.error().path == "/render_resolution");
}

TEST_CASE("FrameKeyMismatch", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-framekey.json");
    requireLoadError(result, dlss::LoadErrorCode::FrameKeyMismatch);
    REQUIRE(result.error().path == "/frames/abc");
}

TEST_CASE("FileNotFound", "[dlssconfig]") {
    auto result = dlss::DlssConfigLoader::Load("dlss-plugin/tests/fixtures/dlssconfig-bad-file.json");
    requireLoadError(result, dlss::LoadErrorCode::FileNotFound);
    REQUIRE(result.error().path.find("dlssconfig-bad-file.json") != std::string::npos);
}
