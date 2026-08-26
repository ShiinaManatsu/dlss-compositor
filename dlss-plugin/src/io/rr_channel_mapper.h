#pragma once
// RrChannelMapper — decode a Blender 5.3 RR multilayer EXR frame into
// GPU-ready typed buffers.
//
// Expected EXR layer / channel naming (from inject.py RR_FILE_ITEMS):
//   Image.R/G/B/A             — noisy scene-linear color  (Combined pass)
//   Denoising Depth.V         — linear view-space depth   (scalar)
//   Vector.X/Y/Z/W            — motion vectors (Blender space; mapper applies -Y flip)
//   Denoising Albedo.R/G/B/A  — diffuse albedo            (Reinhard clamped by mapper)
//   Denoising Specular Albedo.R/G/B/A — specular albedo   (raw, no tonemap)
//   Denoising Normal.X/Y/Z/W  — world-space normals + optional packed roughness in W
//   Denoising Roughness.V     — perceptual roughness (Cycles sqrt(GGX alpha); do NOT sqrt again)
//   Denoising Specular Motion.X/Y/Z/W — specular reflection motion vectors (Blender 5.3+)
//   Denoising Backward Motion.X/Y/Z/W — backward optical flow (not consumed by RR pass,
//                                        stored for possible future use)
//
// Buffer layout (all host-side; caller uploads to GPU via upload helpers):
//   colorRgba         R16G16B16A16_SFLOAT — alpha forced to 1.0
//   depthLinear       R32_SFLOAT
//   motionRg          R16G16_SFLOAT — (Blender.X, -Blender.Y) pixel space
//   diffuseAlbedoRgba R16G16B16A16_SFLOAT — Reinhard tonemapped to [0,1], alpha = 1.0
//   specularAlbedoRgba R16G16B16A16_SFLOAT — raw, alpha = 1.0
//   normalsRgba       R16G16B16A16_SFLOAT — W = roughness if packRoughnessInNormalW, else 0
//   roughnessR32      R32_SFLOAT           — empty when packRoughnessInNormalW
//   specularMotionRg  R16G16_SFLOAT        — (Blender.X, -Blender.Y); empty if not present

#include "../io/frame_prefetcher.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dlss {

struct RrMappedBuffers {
    // ── Core (same contract as SR MappedBuffers) ─────────────────────────
    std::vector<uint16_t> colorRgba;          ///< R16G16B16A16_SFLOAT  — noisy HDR color
    std::vector<float>    depthLinear;         ///< R32_SFLOAT           — linear view-space depth
    std::vector<uint16_t> motionRg;            ///< R16G16_SFLOAT        — (Blender.X, -Blender.Y)

    // ── RR guide buffers ─────────────────────────────────────────────────
    std::vector<uint16_t> diffuseAlbedoRgba;  ///< R16G16B16A16_SFLOAT  — Reinhard-clamped
    std::vector<uint16_t> specularAlbedoRgba; ///< R16G16B16A16_SFLOAT  — raw
    std::vector<uint16_t> normalsRgba;        ///< R16G16B16A16_SFLOAT  — W = roughness if packed
    std::vector<float>    roughnessR32;       ///< R32_SFLOAT           — empty when packed

    // ── Specular motion (Blender 5.3+ pInMotionVectorsReflection) ───────
    std::vector<uint16_t> specularMotionRg;   ///< R16G16_SFLOAT        — empty if not present

    uint32_t width  = 0;
    uint32_t height = 0;

    bool hasSpecularMotion       = false; ///< specularMotionRg populated
    bool roughnessPackedInNormalW = false; ///< normalsRgba.w carries roughness; roughnessR32 empty
};

/// Decode a Blender 5.3 RR multilayer EXR frame (from FramePrefetcher::RrDecodedFrame)
/// into GPU-upload-ready buffers.
///
/// @param frame                  Decoded EXR channels (use RrFramePrefetcher).
/// @param packRoughnessInNormalW Pack roughness into normals.w; leave roughnessR32 empty.
///                               Must match the RrRoughnessMode passed to DlssRrPass::create().
class RrChannelMapper {
public:
    /// Map a decoded RR EXR frame to GPU-ready buffers.
    /// Throws std::runtime_error on missing mandatory channels or dimension mismatch.
    static RrMappedBuffers map(const RrDecodedFrame& frame,
                               bool packRoughnessInNormalW = false);

    // fp32 → fp16 (shared with ChannelMapper; extracted to avoid duplication)
    static uint16_t f32ToF16(float v) noexcept;

private:
    static float reinhard01(float v) noexcept;
    static float finiteOrZero(float v) noexcept;
};

} // namespace dlss
