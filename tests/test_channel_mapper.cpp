#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../src/io/channel_mapper.h"
#include <cstring>
#include <cmath>

using dlss::DecodedFrame;
using dlss::ChannelMapper;
using dlss::MapErrorCode;

static DecodedFrame makeFrame(uint32_t w, uint32_t h, bool hasZ = true, bool hasVector = true) {
    DecodedFrame f;
    f.width = w;
    f.height = h;
    size_t pixels = static_cast<size_t>(w) * h;
    f.combined.resize(pixels * 4, 0.5f);
    if (hasZ)      f.depth.resize(pixels, 1.0f);
    if (hasVector) f.vector.resize(pixels * 4, 0.25f);
    return f;
}

// Helper: decode fp16 back to fp32 for testing
static float f16ToF32(uint16_t h) {
    uint32_t sign = (h >> 15) & 0x1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    if (exp == 0)  return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mant), -24);
    if (exp == 31) return sign ? -INFINITY : INFINITY;
    int e = static_cast<int>(exp) - 15 + 127;
    uint32_t bits = (sign << 31) | (static_cast<uint32_t>(e) << 23) | (mant << 13);
    float v; std::memcpy(&v, &bits, 4);
    return v;
}

TEST_CASE("ChannelMapper - buffer sizes correct", "[unit]") {
    auto frame = makeFrame(3840, 2160);
    auto result = ChannelMapper::map(frame);
    REQUIRE(result.has_value());

    size_t pixels = 3840 * 2160;
    // R16G16B16A16 = 4 channels × 2 bytes = 8 bytes/pixel
    REQUIRE(result->sceneColor.size() == pixels * 4);
    // R32_SFLOAT = 1 channel × 4 bytes = 4 bytes/pixel
    REQUIRE(result->depth.size() == pixels);
    // R16G16B16A16 = 4 channels × 2 bytes
    REQUIRE(result->motionVector.size() == pixels * 4);
    REQUIRE(result->width == 3840);
    REQUIRE(result->height == 2160);
}

TEST_CASE("ChannelMapper - missing depth reports error", "[unit]") {
    auto frame = makeFrame(64, 64, /*hasZ=*/false);
    auto result = ChannelMapper::map(frame);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == MapErrorCode::MissingDepthChannel);
}

TEST_CASE("ChannelMapper - fp32 to fp16 precision", "[unit]") {
    auto frame = makeFrame(4, 4);
    // Set specific values in combined
    frame.combined[0] = 0.5f;
    frame.combined[1] = 0.25f;
    frame.combined[2] = 0.0f;
    frame.combined[3] = 1.0f;

    auto result = ChannelMapper::map(frame);
    REQUIRE(result.has_value());

    // Round-trip fp32 → fp16 → fp32: error < 0.001
    float r0 = f16ToF32(result->sceneColor[0]);
    float r1 = f16ToF32(result->sceneColor[1]);
    REQUIRE(std::abs(r0 - 0.5f) < 0.001f);
    REQUIRE(std::abs(r1 - 0.25f) < 0.001f);
}

TEST_CASE("ChannelMapper - all 4 vector channels preserved", "[unit]") {
    auto frame = makeFrame(4, 4);
    // Set distinct vector channel values
    for (size_t i = 0; i < 16; ++i) {
        frame.vector[i * 4 + 0] = 1.0f;   // X
        frame.vector[i * 4 + 1] = -2.0f;  // Y
        frame.vector[i * 4 + 2] = 3.0f;   // Z
        frame.vector[i * 4 + 3] = -4.0f;  // W
    }

    auto result = ChannelMapper::map(frame);
    REQUIRE(result.has_value());

    // All 4 channels should be preserved (within fp16 precision)
    float x = f16ToF32(result->motionVector[0]);
    float y = f16ToF32(result->motionVector[1]);
    float z = f16ToF32(result->motionVector[2]);
    float w = f16ToF32(result->motionVector[3]);

    REQUIRE(std::abs(x - 1.0f)  < 0.1f);
    REQUIRE(std::abs(y - (-2.0f)) < 0.1f);
    REQUIRE(std::abs(z - 3.0f)  < 0.1f);
    REQUIRE(std::abs(w - (-4.0f)) < 0.1f);
}

TEST_CASE("ChannelMapper - zero dimensions error", "[unit]") {
    DecodedFrame frame;
    frame.width = 0;
    frame.height = 0;
    auto result = ChannelMapper::map(frame);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == MapErrorCode::InvalidDimensions);
}

TEST_CASE("ChannelMapper - missing vector fills zeros", "[unit]") {
    auto frame = makeFrame(8, 8, /*hasZ=*/true, /*hasVector=*/false);
    auto result = ChannelMapper::map(frame);
    REQUIRE(result.has_value());

    // All motion vector entries should be zero
    for (auto v : result->motionVector) {
        REQUIRE(v == 0);
    }
}

TEST_CASE("ChannelMapper - depth values pass through unchanged", "[unit]") {
    auto frame = makeFrame(4, 4);
    // Set specific depth values
    for (size_t i = 0; i < 16; ++i) {
        frame.depth[i] = static_cast<float>(i) * 0.1f;
    }

    auto result = ChannelMapper::map(frame);
    REQUIRE(result.has_value());

    for (size_t i = 0; i < 16; ++i) {
        REQUIRE(result->depth[i] == Catch::Approx(static_cast<float>(i) * 0.1f));
    }
}
