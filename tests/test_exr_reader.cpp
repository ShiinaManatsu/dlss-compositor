#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/exr_reader.h"
#include "core/exr_writer.h"

#include <filesystem>
#include <vector>

static const char* FIXTURE_EXR = "tests/fixtures/reference_64x64.exr";

TEST_CASE("ExrReader - error on missing file", "[exr]") {
    ExrReader reader;
    std::string err;
    bool ok = reader.open("nonexistent.exr", err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("ExrReader - reads fixture channels", "[exr][requires_fixture]") {
    if (!std::filesystem::exists(FIXTURE_EXR)) {
        SKIP("Fixture EXR not yet generated - run tests/generate_fixtures.py first");
    }

    ExrReader reader;
    std::string err;
    bool ok = reader.open(FIXTURE_EXR, err);
    REQUIRE(ok);
    REQUIRE(reader.width() == 64);
    REQUIRE(reader.height() == 64);

    auto channels = reader.listChannels();
    REQUIRE(!channels.empty());

    bool hasR = false;
    for (const auto& ch : channels) {
        if (ch.find(".R") != std::string::npos || ch.find("Combined.R") != std::string::npos || ch == "R") {
            hasR = true;
        }
    }
    REQUIRE(hasR);
}

TEST_CASE("ExrWriter - round trip", "[exr]") {
    const int W = 4;
    const int H = 4;
    std::vector<float> data(static_cast<size_t>(W * H));
    for (int i = 0; i < W * H; ++i) {
        data[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(W * H);
    }

    std::filesystem::create_directories("tests/fixtures");
    std::string tmpPath = "tests/fixtures/tmp_roundtrip.exr";

    ExrWriter writer;
    std::string err;
    bool ok = writer.create(tmpPath, W, H, err);
    REQUIRE(ok);
    ok = writer.addChannel("R", data.data());
    REQUIRE(ok);
    ok = writer.write(err);
    REQUIRE(ok);

    ExrReader reader;
    ok = reader.open(tmpPath, err);
    REQUIRE(ok);
    REQUIRE(reader.width() == W);
    REQUIRE(reader.height() == H);

    const float* readData = reader.readChannel("R");
    REQUIRE(readData != nullptr);
    REQUIRE(readData[0] == Catch::Approx(data[0]).epsilon(0.01));
    REQUIRE(readData[W * H - 1] == Catch::Approx(data[static_cast<size_t>(W * H - 1)]).epsilon(0.01));

    std::filesystem::remove(tmpPath);
}

TEST_CASE("ExrReader - legacy aliases resolve Blender 5 channel names", "[exr]") {
    const int W = 2;
    const int H = 2;
    std::vector<float> data{0.0f, 0.25f, 0.5f, 1.0f};

    std::filesystem::create_directories("tests/fixtures");
    const std::string tmpPath = "tests/fixtures/tmp_alias_roundtrip.exr";

    ExrWriter writer;
    std::string err;
    REQUIRE(writer.create(tmpPath, W, H, err));
    REQUIRE(writer.addChannel("Image.R", data.data()));
    REQUIRE(writer.write(err));

    ExrReader reader;
    REQUIRE(reader.open(tmpPath, err));
    REQUIRE(reader.readChannel("Image.R") != nullptr);

    const float* aliasData = reader.readChannel("RenderLayer.Combined.R");
    REQUIRE(aliasData != nullptr);
    REQUIRE(aliasData[0] == Catch::Approx(data[0]).epsilon(0.01));
    REQUIRE(aliasData[3] == Catch::Approx(data[3]).epsilon(0.01));

    std::filesystem::remove(tmpPath);
}
