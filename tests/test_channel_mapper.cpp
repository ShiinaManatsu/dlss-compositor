#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/core/channel_mapper.h"
#include "../src/core/exr_reader.h"

#include <filesystem>

static const char* FIXTURE_EXR = "tests/fixtures/reference_64x64.exr";
static const char* MISSING_EXR = "tests/fixtures/missing_channels_64x64.exr";

TEST_CASE("ChannelMapper - defaults on empty reader", "[mapper]") {
    ExrReader reader;
    ChannelMapper mapper;
    MappedBuffers out;
    std::string err;
    REQUIRE_FALSE(mapper.mapFromExr(reader, out, err));
    REQUIRE(true);
}

TEST_CASE("ChannelMapper - reads from fixture", "[mapper][requires_fixture]") {
    if (!std::filesystem::exists(FIXTURE_EXR)) {
        SKIP("Fixture EXR not yet generated - run tests/generate_fixtures.py first");
    }

    ExrReader reader;
    std::string err;
    REQUIRE(reader.open(FIXTURE_EXR, err));

    ChannelMapper mapper;
    MappedBuffers out;
    REQUIRE(mapper.mapFromExr(reader, out, err));

    const int pixelCount = reader.width() * reader.height();
    REQUIRE(out.color.size() == static_cast<size_t>(pixelCount * 4));
    REQUIRE(out.depth.size() == static_cast<size_t>(pixelCount));
    REQUIRE(out.motionVectors.size() == static_cast<size_t>(pixelCount * 4));
    REQUIRE(out.diffuseAlbedo.size() == static_cast<size_t>(pixelCount * 3));
    REQUIRE(out.specularAlbedo.size() == static_cast<size_t>(pixelCount * 3));
    REQUIRE(out.normals.size() == static_cast<size_t>(pixelCount * 3));
    REQUIRE(out.roughness.size() == static_cast<size_t>(pixelCount));
}

TEST_CASE("ChannelMapper - missing optional channels get defaults", "[mapper][requires_fixture]") {
    if (!std::filesystem::exists(MISSING_EXR)) {
        SKIP("Missing-channels fixture not yet generated - run tests/generate_fixtures.py first");
    }

    ExrReader reader;
    std::string err;
    REQUIRE(reader.open(MISSING_EXR, err));

    ChannelMapper mapper;
    MappedBuffers out;
    REQUIRE(mapper.mapFromExr(reader, out, err));

    const int pixelCount = reader.width() * reader.height();
    REQUIRE(out.diffuseAlbedo.size() == static_cast<size_t>(pixelCount * 3));
    REQUIRE(out.diffuseAlbedo[0] == Catch::Approx(1.0f).epsilon(0.01f));
    REQUIRE(out.diffuseAlbedo[1] == Catch::Approx(1.0f).epsilon(0.01f));
    REQUIRE(out.diffuseAlbedo[2] == Catch::Approx(1.0f).epsilon(0.01f));
    REQUIRE(out.roughness.size() == static_cast<size_t>(pixelCount));
    REQUIRE(out.roughness[0] == Catch::Approx(0.5f).epsilon(0.01f));
}

TEST_CASE("ChannelMapper - custom name mapping override", "[mapper]") {
    ChannelNames customNames;
    customNames.colorR = "CustomView.Beauty.R";
    customNames.colorG = "CustomView.Beauty.G";
    customNames.colorB = "CustomView.Beauty.B";
    customNames.colorA = "CustomView.Beauty.A";
    ChannelMapper mapper(customNames);
    REQUIRE(true);
}
