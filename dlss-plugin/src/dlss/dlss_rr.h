#pragma once
// DLSS Ray Reconstruction (DLSS-RR) pass — Vulkan.
//
// Wraps NGX_VULKAN_CREATE_DLSSD_EXT / NGX_VULKAN_EVALUATE_DLSSD_EXT
// (nvsdk_ngx_helpers_dlssd_vk.h).
//
// RR replaces SR: it takes the same core buffers (color, depth, MV) plus
// mandatory guide buffers (diffuse albedo, specular albedo, normals, roughness)
// and the new Blender 5.3 specular motion vectors.
//
// Usage mirrors DlssSrPass:
//   1. Construct with NgxContext + VulkanContext + TexturePool.
//   2. Call create() once with resolutions + quality + preset.
//   3. Call evaluate() per frame inside the RR+FG pipeline.
//   4. Destroy is called automatically in the destructor.

#include <vulkan/vulkan.h>
#include "../core/types.h"
#include "ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace dlss {

/// Per-frame RR inputs.  All image views are at render resolution unless noted.
struct RrInputs {
    // ── Core buffers (same as SR) ────────────────────────────────────────
    VkImageView colorRenderRes;   ///< Noisy scene-linear HDR at render res.  R16G16B16A16_SFLOAT
    VkImageView outputRes;        ///< RR output at target (output) res.       R16G16B16A16_SFLOAT
    VkImageView depthLinear;      ///< Linear view-space depth.                R32_SFLOAT
    VkImageView motionVectors;    ///< Camera + object motion, pixel space.   R16G16_SFLOAT
    VkImage     colorImage;
    VkImage     outputImage;
    VkImage     depthImage;
    VkImage     motionImage;

    // ── RR guide buffers (mandatory, from Blender 5.3 denoising passes) ─
    VkImageView diffuseAlbedo;    ///< Denoising Albedo (Reinhard-clamped).   R16G16B16A16_SFLOAT
    VkImageView specularAlbedo;   ///< Denoising Specular Albedo.             R16G16B16A16_SFLOAT
    VkImageView normals;          ///< Denoising Normal (world-space, norm'd). R16G16B16A16_SFLOAT (w = roughness if Packed)
    VkImageView roughness;        ///< Denoising Roughness.  nullptr if Packed. R32_SFLOAT
    VkImage     diffuseAlbedoImage;
    VkImage     specularAlbedoImage;
    VkImage     normalsImage;
    VkImage     roughnessImage;   ///< VK_NULL_HANDLE when Packed

    // ── Specular motion (Blender 5.3+ pInMotionVectorsReflection) ───────
    VkImageView specularMotion;   ///< Denoising Specular Motion, pixel space. R16G16_SFLOAT
    VkImage     specularMotionImage;
    bool        hasSpecularMotion = false;  ///< false → skip pInMotionVectorsReflection

    // ── Frame parameters ─────────────────────────────────────────────────
    uint32_t renderW;
    uint32_t renderH;
    uint32_t outputW;
    uint32_t outputH;
    float    jitterX;             ///< Pixel-space jitter from dlssconfig, same sign as SR.
    float    jitterY;
    bool     reset;               ///< First frame, gap, or camera cut.  DLSS §3.13
    float    frameTimeDeltaMs;    ///< 1000.0f / fps — never hardcoded.  DLSS §3.14
};

/// RR preset.  D and E are production-ready; F is RR2 (driver >= 580, 310.7.128+ DLL).
enum class RrPreset { D, E, F };

/// Whether roughness is stored separately or packed into normals.w.
enum class RrRoughnessMode { Unpacked, Packed };

/// DLSS Ray Reconstruction pass (Vulkan NGX).
/// Thread-hostile: do not call from multiple threads concurrently.
class DlssRrPass {
public:
    DlssRrPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool);
    ~DlssRrPass();

    DlssRrPass(const DlssRrPass&)            = delete;
    DlssRrPass& operator=(const DlssRrPass&) = delete;

    /// Create the NGX RR feature.  Must be called before the first evaluate().
    /// @param presetOverride  RrPreset::E is the recommended default.
    void create(uint32_t renderW,
                uint32_t renderH,
                uint32_t outputW,
                uint32_t outputH,
                QualityMode quality,
                RrPreset    preset       = RrPreset::E,
                RrRoughnessMode roughness = RrRoughnessMode::Unpacked);

    /// Evaluate RR for one frame.  Submit cmd before this returns.
    void evaluate(VkCommandBuffer cmd, const RrInputs& in);

    /// Release the NGX RR feature handle (safe to call multiple times).
    void destroy();

    bool isCreated() const { return m_handle != nullptr; }

    // Read-back after create()
    uint32_t optimalWidth()  const { return m_optimalW; }
    uint32_t optimalHeight() const { return m_optimalH; }
    uint32_t minWidth()      const { return m_minW; }
    uint32_t minHeight()     const { return m_minH; }
    uint32_t maxWidth()      const { return m_maxW; }
    uint32_t maxHeight()     const { return m_maxH; }

private:
    static int toNgxQuality(QualityMode q);
    void setPreset(NVSDK_NGX_Parameter* params, RrPreset preset);

    NgxContext*    m_ngx;
    VulkanContext* m_ctx;
    TexturePool*   m_pool;

    NVSDK_NGX_Handle*    m_handle     = nullptr;
    NVSDK_NGX_Parameter* m_ngxParams  = nullptr;

    RrRoughnessMode m_roughnessMode   = RrRoughnessMode::Unpacked;

    uint32_t m_optimalW = 0;
    uint32_t m_optimalH = 0;
    uint32_t m_minW     = 0;
    uint32_t m_minH     = 0;
    uint32_t m_maxW     = 0;
    uint32_t m_maxH     = 0;
};

} // namespace dlss
