#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/exr_reader.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kWidth = 2;
constexpr int kHeight = 2;
const std::filesystem::path kReferenceExr = "tests/fixtures/reference_64x64.exr";

using ChannelPixels = std::pair<std::string, std::vector<float>>;

std::filesystem::path tempAliasPath(const std::string& filename) {
    const std::filesystem::path tempDir = std::filesystem::path("tests/fixtures/tmp_exr_channel_aliases");
    std::filesystem::create_directories(tempDir);
    return tempDir / filename;
}

std::filesystem::path findFixture(const std::filesystem::path& relativePath) {
    const std::vector<std::filesystem::path> roots = {
        std::filesystem::current_path(),
        std::filesystem::current_path().parent_path(),
        std::filesystem::current_path().parent_path().parent_path(),
    };
    for (const auto& root : roots) {
        const std::filesystem::path candidate = root / relativePath;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return relativePath;
}

void writeFloatExr(const std::filesystem::path& path, std::vector<ChannelPixels> channels) {
    Imf::Header header(kWidth, kHeight);
    for (const auto& [name, pixels] : channels) {
        REQUIRE(pixels.size() == static_cast<size_t>(kWidth * kHeight));
        header.channels().insert(name.c_str(), Imf::Channel(Imf::FLOAT));
    }

    Imf::FrameBuffer frameBuffer;
    for (auto& [name, pixels] : channels) {
        frameBuffer.insert(name.c_str(),
                           Imf::Slice(Imf::FLOAT,
                                      reinterpret_cast<char*>(pixels.data()),
                                      sizeof(float),
                                      sizeof(float) * static_cast<size_t>(kWidth)));
    }

    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(kHeight);
}

void writeStandardExr(const std::filesystem::path& path) {
    writeFloatExr(path,
                  {{"R", {0.5f, 0.51f, 0.52f, 0.53f}},
                   {"G", {0.6f, 0.61f, 0.62f, 0.63f}},
                   {"B", {0.7f, 0.71f, 0.72f, 0.73f}},
                   {"A", {1.0f, 1.0f, 1.0f, 1.0f}},
                   {"Z", {5.0f, 5.1f, 5.2f, 5.3f}}});
}

void writeMvExr(const std::filesystem::path& path) {
    writeFloatExr(path,
                  {{"R", {0.3f, 0.31f, 0.32f, 0.33f}},
                   {"G", {0.4f, 0.41f, 0.42f, 0.43f}}});
}

} // namespace

TEST_CASE("ExrChannelAliases standard EXR channels map to canonical names", "[exr][ExrChannelAliases]") {
    const std::filesystem::path exrPath = tempAliasPath("standard.exr");
    writeStandardExr(exrPath);

    ExrReader reader;
    std::string error;
    REQUIRE(reader.open(exrPath.string(), error));
    REQUIRE(reader.width() == kWidth);
    REQUIRE(reader.height() == kHeight);

    const float* red = reader.readChannel("RenderLayer.Combined.R");
    const float* alpha = reader.readChannel("RenderLayer.Combined.A");
    const float* depth = reader.readChannel("RenderLayer.Depth.Z");

    REQUIRE(red != nullptr);
    REQUIRE(alpha != nullptr);
    REQUIRE(depth != nullptr);
    CHECK(red[0] == Catch::Approx(0.5f));
    CHECK(alpha[3] == Catch::Approx(1.0f));
    CHECK(depth[2] == Catch::Approx(5.2f));

    std::filesystem::remove(exrPath);
}

TEST_CASE("ExrChannelAliases merges standard MV sibling channels", "[exr][ExrChannelAliases]") {
    const std::filesystem::path exrPath = tempAliasPath("frame_0001.exr");
    const std::filesystem::path mvPath = tempAliasPath("frame_0001.mv.exr");
    writeStandardExr(exrPath);
    writeMvExr(mvPath);

    ExrReader reader;
    std::string error;
    REQUIRE(reader.open(exrPath.string(), error));
    REQUIRE(reader.mergeMvFile(mvPath.string(), error));

    const float* mvX = reader.readChannel("RenderLayer.Vector.X");
    const float* mvY = reader.readChannel("RenderLayer.Vector.Y");

    REQUIRE(mvX != nullptr);
    REQUIRE(mvY != nullptr);
    CHECK(mvX[0] == Catch::Approx(0.3f));
    CHECK(mvY[3] == Catch::Approx(0.43f));

    std::filesystem::remove(exrPath);
    std::filesystem::remove(mvPath);
}

TEST_CASE("ExrChannelAliases missing MV sibling fails gracefully", "[exr][ExrChannelAliases]") {
    const std::filesystem::path exrPath = tempAliasPath("missing_mv.exr");
    const std::filesystem::path mvPath = tempAliasPath("missing_mv.mv.exr");
    writeStandardExr(exrPath);
    std::filesystem::remove(mvPath);

    ExrReader reader;
    std::string error;
    REQUIRE(reader.open(exrPath.string(), error));
    REQUIRE_FALSE(reader.mergeMvFile(mvPath.string(), error));
    REQUIRE_FALSE(error.empty());
    CHECK(reader.readChannel("RenderLayer.Vector.X") == nullptr);
    CHECK(reader.readChannel("RenderLayer.Vector.Y") == nullptr);

    std::filesystem::remove(exrPath);
}

TEST_CASE("ExrChannelAliases MultiLayer aliases remain backward compatible", "[exr][ExrChannelAliases][requires_fixture]") {
    const std::filesystem::path referenceExr = findFixture(kReferenceExr);
    if (!std::filesystem::exists(referenceExr)) {
        SKIP("Reference EXR fixture not found - run tests/generate_fixtures.py first");
    }

    ExrReader reader;
    std::string error;
    REQUIRE(reader.open(referenceExr.string(), error));

    REQUIRE(reader.readChannel("RenderLayer.Combined.R") != nullptr);
    REQUIRE(reader.readChannel("RenderLayer.Depth.Z") != nullptr);
    REQUIRE(reader.readChannel("RenderLayer.Vector.X") != nullptr);
    REQUIRE(reader.readChannel("RenderLayer.Vector.Y") != nullptr);
}
