// RrChannelMapper implementation.
//
// Buffer encoding notes (from d3d12-rr experiment, rr_guides.cpp, validated):
//   - colorRgba:         scene-linear HDR fp32→fp16; alpha forced to 1.0
//   - diffuseAlbedoRgba: Reinhard(x/(1+x)) clamped to [0,1]; helps RR stability
//   - specularAlbedoRgba: raw (no tonemap); alpha = 1.0
//   - normalsRgba:       world-space XYZ → fp16 RGBA; W = roughness if Packed, else 0
//   - roughnessR32:      Cycles perceptual = sqrt(GGX alpha) already in EXR;
//                        do NOT sqrt again.  NOT Principled roughness input.
//   - motionRg:          (Blender.X, -Blender.Y) — Y sign flip here, MV scale stays 1.0
//   - specularMotionRg:  same sign convention as motionRg

#include "rr_channel_mapper.h"

#include <bit>
#include <cmath>
#include <stdexcept>

namespace dlss {

namespace {

// ---------------------------------------------------------------------------
// Float → Half  (IEEE 754, round-to-nearest-even)
// ---------------------------------------------------------------------------
// Using std::bit_cast (C++20) for the fp32 bit extraction.
uint16_t floatToHalf(float f) noexcept {
    const uint32_t bits = std::bit_cast<uint32_t>(f);
    const uint32_t sign     = (bits >> 31) & 0x1u;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    const uint32_t mantissa = bits & 0x7FFFFFu;

    uint16_t hSign = (uint16_t)(sign << 15);

    if (exponent == 0xFF) {
        // NaN or Inf
        const uint16_t hMant = mantissa ? (uint16_t)0x0200u : (uint16_t)0u;
        return (uint16_t)(hSign | 0x7C00u | hMant);
    }
    const int e = (int)exponent - 127 + 15;
    if (e >= 31) {
        return (uint16_t)(hSign | 0x7C00u); // Inf
    }
    if (e <= 0) {
        if (e < -10) return hSign; // Underflow → signed zero
        const uint16_t hMant = (uint16_t)((mantissa | 0x800000u) >> (14 - e));
        return (uint16_t)(hSign | hMant);
    }
    return (uint16_t)(hSign | (uint16_t)(e << 10) | (uint16_t)(mantissa >> 13));
}

} // namespace

// Public static helpers
uint16_t RrChannelMapper::f32ToF16(float v) noexcept { return floatToHalf(v); }

float RrChannelMapper::reinhard01(float v) noexcept {
    v = finiteOrZero(v);
    if (v < 0.0f) v = 0.0f;
    return v / (1.0f + v); // → [0,1)
}

float RrChannelMapper::finiteOrZero(float v) noexcept {
    return std::isfinite(v) ? v : 0.0f;
}

// ──────────────────────────────────────────────────────────────────────────────

RrMappedBuffers RrChannelMapper::map(const RrDecodedFrame& frame, bool packRoughnessInNormalW) {
    if (frame.width == 0 || frame.height == 0) {
        throw std::runtime_error("RrChannelMapper::map: empty frame");
    }
    const std::size_t pixels = (std::size_t)frame.width * frame.height;

    // Validate mandatory buffer sizes
    if (frame.colorRgba.size()        < pixels * 4 ||
        frame.depthLinear.size()      < pixels     ||
        frame.motionXy.size()         < pixels * 2 ||
        frame.diffuseAlbedoRgba.size()< pixels * 4 ||
        frame.specAlbedoRgba.size()   < pixels * 4 ||
        frame.normalsXyz.size()       < pixels * 3 ||
        frame.roughness.size()        < pixels) {
        throw std::runtime_error("RrChannelMapper::map: decoded frame has undersized buffers");
    }

    const uint16_t kOne  = floatToHalf(1.0f);
    const uint16_t kZero = floatToHalf(0.0f);

    RrMappedBuffers out;
    out.width  = frame.width;
    out.height = frame.height;
    out.roughnessPackedInNormalW = packRoughnessInNormalW;
    out.hasSpecularMotion        = frame.hasSpecularMotion &&
                                   frame.specularMotionXy.size() >= pixels * 2;

    out.colorRgba.resize(pixels * 4);
    out.depthLinear.resize(pixels);
    out.motionRg.resize(pixels * 2);
    out.diffuseAlbedoRgba.resize(pixels * 4);
    out.specularAlbedoRgba.resize(pixels * 4);
    out.normalsRgba.resize(pixels * 4);
    if (!packRoughnessInNormalW) {
        out.roughnessR32.resize(pixels);
    }
    if (out.hasSpecularMotion) {
        out.specularMotionRg.resize(pixels * 2);
    }

    const float* colSrc    = frame.colorRgba.data();
    const float* depSrc    = frame.depthLinear.data();
    const float* mvSrc     = frame.motionXy.data();
    const float* diffSrc   = frame.diffuseAlbedoRgba.data();
    const float* specSrc   = frame.specAlbedoRgba.data();
    const float* normSrc   = frame.normalsXyz.data();
    const float* roughSrc  = frame.roughness.data();
    const float* smvSrc    = out.hasSpecularMotion ? frame.specularMotionXy.data() : nullptr;

    uint16_t* colDst    = out.colorRgba.data();
    float*    depDst    = out.depthLinear.data();
    uint16_t* mvDst     = out.motionRg.data();
    uint16_t* diffDst   = out.diffuseAlbedoRgba.data();
    uint16_t* specDst   = out.specularAlbedoRgba.data();
    uint16_t* normDst   = out.normalsRgba.data();
    float*    roughDst  = packRoughnessInNormalW ? nullptr : out.roughnessR32.data();
    uint16_t* smvDst    = out.hasSpecularMotion ? out.specularMotionRg.data() : nullptr;

    for (std::size_t i = 0; i < pixels; ++i) {
        // Color: scene-linear HDR; alpha = 1.0
        colDst[i*4+0] = floatToHalf(finiteOrZero(colSrc[i*4+0]));
        colDst[i*4+1] = floatToHalf(finiteOrZero(colSrc[i*4+1]));
        colDst[i*4+2] = floatToHalf(finiteOrZero(colSrc[i*4+2]));
        colDst[i*4+3] = kOne;

        // Depth: pass through as float32 (linear from Blender)
        depDst[i] = finiteOrZero(depSrc[i]);

        // Motion vectors: DLSS_MV = (Blender.X, -Blender.Y)  — only Y flipped
        mvDst[i*2+0] = floatToHalf( finiteOrZero(mvSrc[i*2+0]));
        mvDst[i*2+1] = floatToHalf(-finiteOrZero(mvSrc[i*2+1]));

        // Diffuse albedo: Reinhard-clamped to [0,1]; alpha = 1.0
        // Prevents very bright sky/emissive pixels from dominating the guide.
        diffDst[i*4+0] = floatToHalf(reinhard01(diffSrc[i*4+0]));
        diffDst[i*4+1] = floatToHalf(reinhard01(diffSrc[i*4+1]));
        diffDst[i*4+2] = floatToHalf(reinhard01(diffSrc[i*4+2]));
        diffDst[i*4+3] = kOne;

        // Specular albedo: raw (no tonemap); alpha = 1.0
        specDst[i*4+0] = floatToHalf(finiteOrZero(specSrc[i*4+0]));
        specDst[i*4+1] = floatToHalf(finiteOrZero(specSrc[i*4+1]));
        specDst[i*4+2] = floatToHalf(finiteOrZero(specSrc[i*4+2]));
        specDst[i*4+3] = kOne;

        // Normals: world-space XYZ.  W = roughness if Packed, else 0.
        const float rough = finiteOrZero(roughSrc[i]);
        normDst[i*4+0] = floatToHalf(finiteOrZero(normSrc[i*3+0]));
        normDst[i*4+1] = floatToHalf(finiteOrZero(normSrc[i*3+1]));
        normDst[i*4+2] = floatToHalf(finiteOrZero(normSrc[i*3+2]));
        normDst[i*4+3] = packRoughnessInNormalW ? floatToHalf(rough) : kZero;

        // Roughness: Cycles perceptual (sqrt(GGX alpha) is already in the EXR).
        // Do NOT sqrt again — also not the Principled roughness input.
        if (roughDst) {
            roughDst[i] = rough;
        }

        // Specular motion: (Blender.X, -Blender.Y) same convention as main MV
        if (smvDst && smvSrc) {
            smvDst[i*2+0] = floatToHalf( finiteOrZero(smvSrc[i*2+0]));
            smvDst[i*2+1] = floatToHalf(-finiteOrZero(smvSrc[i*2+1]));
        }
    }

    return out;
}

} // namespace dlss
