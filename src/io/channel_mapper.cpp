#include "channel_mapper.h"
#include <cstring>
#include <cmath>
#include <stdexcept>

namespace dlss {

// Simple fp32 → fp16 conversion using bit manipulation
// Handles normal numbers, subnormals, infinity, and NaN
uint16_t ChannelMapper::f32ToF16(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);

    uint32_t sign     = (bits >> 31) & 0x1;
    uint32_t exponent = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x7FFFFF;

    // Handle special cases
    if (exponent == 255) {
        // NaN or Infinity → map to fp16 infinity or NaN
        return static_cast<uint16_t>((sign << 15) | 0x7C00 | (mantissa ? 0x200 : 0));
    }
    if (exponent == 0) {
        // Denormal → 0
        return static_cast<uint16_t>(sign << 15);
    }

    // Bias difference: fp32 bias=127, fp16 bias=15
    int e = static_cast<int>(exponent) - 127 + 15;

    if (e >= 31) {
        // Overflow → infinity
        return static_cast<uint16_t>((sign << 15) | 0x7C00);
    }
    if (e <= 0) {
        // Underflow → 0 or denormal
        if (e < -10) {
            return static_cast<uint16_t>(sign << 15);
        }
        // Denormal fp16
        uint32_t m = (mantissa | 0x800000) >> (1 - e);
        return static_cast<uint16_t>((sign << 15) | (m >> 13));
    }

    // Normal case
    uint16_t h_exp = static_cast<uint16_t>(e) << 10;
    uint16_t h_man = static_cast<uint16_t>(mantissa >> 13);
    return static_cast<uint16_t>((sign << 15) | h_exp | h_man);
}

MapResult ChannelMapper::map(const DecodedFrame& frame) {
    if (frame.width == 0 || frame.height == 0) {
        return std::unexpected(MapError{
            MapErrorCode::InvalidDimensions,
            "Frame has zero width or height"
        });
    }

    size_t pixelCount = static_cast<size_t>(frame.width) * frame.height;

    // Validate combined (RGBA) — 4 floats per pixel
    if (frame.combined.empty() || frame.combined.size() < pixelCount * 4) {
        return std::unexpected(MapError{
            MapErrorCode::MissingColorChannel,
            "Combined RGBA channel data is missing or incomplete"
        });
    }

    // Validate depth (Z) — 1 float per pixel
    if (frame.depth.empty() || frame.depth.size() < pixelCount) {
        return std::unexpected(MapError{
            MapErrorCode::MissingDepthChannel,
            "Depth (Z) channel data is missing or incomplete"
        });
    }

    MappedBuffers result;
    result.width  = frame.width;
    result.height = frame.height;

    // --- Scene Color: R16G16B16A16_SFLOAT ---
    result.sceneColor.resize(pixelCount * 4);
    for (size_t i = 0; i < pixelCount * 4; ++i) {
        result.sceneColor[i] = f32ToF16(frame.combined[i]);
    }

    // --- Depth: R32_SFLOAT (raw, no NDC conversion — T22 does that on GPU) ---
    result.depth.resize(pixelCount);
    std::copy(frame.depth.begin(), frame.depth.begin() + static_cast<std::ptrdiff_t>(pixelCount), result.depth.begin());

    // --- Motion Vector: R16G16B16A16_SFLOAT (all 4 Blender channels preserved) ---
    // T23 shader will apply: DLSS_MV = (Blender.X, -Blender.Y)
    // We preserve all 4 channels so the shader has everything it needs
    result.motionVector.resize(pixelCount * 4);
    if (!frame.vector.empty() && frame.vector.size() >= pixelCount * 4) {
        for (size_t i = 0; i < pixelCount * 4; ++i) {
            result.motionVector[i] = f32ToF16(frame.vector[i]);
        }
    } else {
        // Vector pass missing — fill with zeros (no motion)
        std::fill(result.motionVector.begin(), result.motionVector.end(), uint16_t{0});
    }

    return result;
}

} // namespace dlss
