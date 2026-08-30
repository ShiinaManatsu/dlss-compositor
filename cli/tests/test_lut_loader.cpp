#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/lut_loader.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path writeTempCube(const std::string& fileName, const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() / fileName;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    output.close();
    return path;
}

} // namespace

TEST_CASE("LutLoader loads 2x2x2 cube", "[lut]") {
    const auto path = writeTempCube("dlss-lut-loader-valid.cube", R"(# comment
TITLE "identity-ish"
LUT_3D_SIZE 2
DOMAIN_MIN 0.0 0.0 0.0
DOMAIN_MAX 1.0 1.0 1.0
0.0 0.0 0.0
0.0 0.0 1.0
0.0 1.0 0.0
0.0 1.0 1.0
1.0 0.0 0.0
1.0 0.0 1.0
1.0 1.0 0.0
1.0 1.0 1.0
)");

    auto result = dlss::LutLoader::LoadCube(path);

    REQUIRE(result.has_value());
    REQUIRE(result->size == 2);
    REQUIRE(result->rgb.size() == 24);
    REQUIRE(result->domain_min[0] == Catch::Approx(0.0f));
    REQUIRE(result->domain_max[2] == Catch::Approx(1.0f));
    REQUIRE(result->rgb.front() == Catch::Approx(0.0f));
    REQUIRE(result->rgb.back() == Catch::Approx(1.0f));
}

TEST_CASE("LutLoader reads DLSS log2 shaper metadata", "[lut]") {
    const auto path = writeTempCube("dlss-lut-loader-log2.cube", R"(TITLE "log2"
# DLSS_LUT_INPUT_ENCODING log2
# DLSS_LUT_LOG2_MIN -10
# DLSS_LUT_LOG2_MAX 6
LUT_3D_SIZE 2
DOMAIN_MIN 0.0 0.0 0.0
DOMAIN_MAX 1.0 1.0 1.0
0.0 0.0 0.0
0.0 0.0 1.0
0.0 1.0 0.0
0.0 1.0 1.0
1.0 0.0 0.0
1.0 0.0 1.0
1.0 1.0 0.0
1.0 1.0 1.0
)");

    auto result = dlss::LutLoader::LoadCube(path);

    REQUIRE(result.has_value());
    REQUIRE(result->input_log2);
    REQUIRE(result->log2_min == Catch::Approx(-10.0f));
    REQUIRE(result->log2_max == Catch::Approx(6.0f));
}

TEST_CASE("LutLoader rejects data count mismatch", "[lut]") {
    const auto path = writeTempCube("dlss-lut-loader-short.cube", R"(LUT_3D_SIZE 2
0.0 0.0 0.0
)");

    auto result = dlss::LutLoader::LoadCube(path);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dlss::LutLoadErrorCode::BadFormat);
}

TEST_CASE("LutLoader rejects 1D cube", "[lut]") {
    const auto path = writeTempCube("dlss-lut-loader-1d.cube", R"(LUT_1D_SIZE 2
0.0 0.0 0.0
1.0 1.0 1.0
)");

    auto result = dlss::LutLoader::LoadCube(path);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dlss::LutLoadErrorCode::Unsupported);
}
