#include "dlss_rr.h"
#include "../core/log.h"

// DLSS RR Vulkan headers
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <stdexcept>
#include <string>

namespace dlss {

namespace {

/// Build an NGX Vulkan resource descriptor for a 2-D color attachment.
NVSDK_NGX_Resource_VK makeRrResource(VkImageView view,
                                     VkImage     image,
                                     uint32_t    width,
                                     uint32_t    height,
                                     VkFormat    format,
                                     bool        readWrite) {
    VkImageSubresourceRange range{};
    range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel   = 0;
    range.levelCount     = 1;
    range.baseArrayLayer = 0;
    range.layerCount     = 1;
    // DLSS SR §3.4 — NGX VK resources need both the view and the backing image.
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        view, image, range, format, width, height, readWrite);
}

} // namespace

// ──────────────────────────────────────────────────────────────────────────────

DlssRrPass::DlssRrPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool)
    : m_ngx(ngx), m_ctx(ctx), m_pool(pool)
{
    if (!m_ngx || !m_ctx || !m_pool) {
        throw std::invalid_argument("DlssRrPass: NgxContext, VulkanContext, TexturePool must not be null");
    }
    // Reuse the shared capability params from NgxContext (same object as SR/FG).
    m_ngxParams = m_ngx->params();
}

DlssRrPass::~DlssRrPass() {
    destroy();
}

// ──────────────────────────────────────────────────────────────────────────────
// create()

int DlssRrPass::toNgxQuality(QualityMode q) {
    // DLSS §3.8 — quality enum map (same values used by both SR and RR).
    switch (q) {
    case QualityMode::DLAA:             return NVSDK_NGX_PerfQuality_Value_DLAA;
    case QualityMode::UltraQuality:     return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case QualityMode::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case QualityMode::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
    case QualityMode::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case QualityMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                            return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

void DlssRrPass::setPreset(NVSDK_NGX_Parameter* params, RrPreset preset) {
    // nvsdk_ngx_defs_dlssd.h — set the same preset value across all quality slots.
    // D = default transformer; E = latest transformer (required for DoF guide);
    // F = RR2 on 310.7.128+ DLL with driver >= 580.
    NVSDK_NGX_RayReconstruction_Hint_Render_Preset ngxPreset;
    switch (preset) {
    case RrPreset::F:
        ngxPreset = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F; // DLSS-RR §preset — RR2
        break;
    case RrPreset::E:
        ngxPreset = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E; // DLSS-RR §preset — latest transformer
        break;
    default: // D
        ngxPreset = NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D; // DLSS-RR §preset — default transformer
        break;
    }
    const unsigned v = static_cast<unsigned>(ngxPreset);
    // Apply to all quality slots so the same preset is used regardless of quality mode.
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,             v);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,     v);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,          v);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,         v);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,      v);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, v);
}

void DlssRrPass::create(uint32_t   renderW,
                        uint32_t   renderH,
                        uint32_t   outputW,
                        uint32_t   outputH,
                        QualityMode quality,
                        RrPreset    preset,
                        RrRoughnessMode roughness) {
    if (m_handle) destroy();

    const int ngxQuality = toNgxQuality(quality);

    // nvsdk_ngx_helpers_dlssd.h — NGX_DLSSD_GET_OPTIMAL_SETTINGS (not the SR variant).
    uint32_t optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float    unusedSharpness = 0.0f;
    NVSDK_NGX_Result r = NGX_DLSSD_GET_OPTIMAL_SETTINGS( // DLSS-RR §3.8
        m_ngxParams,
        outputW, outputH,
        static_cast<NVSDK_NGX_PerfQuality_Value>(ngxQuality),
        &optW, &optH, &maxW, &maxH, &minW, &minH,
        &unusedSharpness);
    if (NVSDK_NGX_FAILED(r)) {
        throw std::runtime_error("NGX_DLSSD_GET_OPTIMAL_SETTINGS failed: " + std::to_string(r));
    }
    if (renderW < minW || renderW > maxW || renderH < minH || renderH > maxH) {
        throw std::runtime_error(
            "RR render resolution " + std::to_string(renderW) + "x" + std::to_string(renderH) +
            " outside NGX range " + std::to_string(minW) + "x" + std::to_string(minH) +
            ".." + std::to_string(maxW) + "x" + std::to_string(maxH));
    }
    m_optimalW = optW;  m_optimalH = optH;
    m_minW     = minW;  m_minH     = minH;
    m_maxW     = maxW;  m_maxH     = maxH;

    // Preset must be set before NGX_VULKAN_CREATE_DLSSD_EXT.
    setPreset(m_ngxParams, preset);

    // nvsdk_ngx_params_dlssd.h — DLSSD_Create_Params
    NVSDK_NGX_DLSSD_Create_Params cp{};
    cp.InDenoiseMode   = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified; // DLSS-RR §2.1 — unified denoiser
    cp.InUseHWDepth    = NVSDK_NGX_DLSS_Depth_Type_Linear;       // DLSS-RR §2.3 — linear depth from Blender
    m_roughnessMode    = roughness;
    if (roughness == RrRoughnessMode::Packed) {
        cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;   // roughness in normals.w
    } else {
        cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked; // separate pInRoughness buffer
    }
    // Create at the actual render size, not the optimal size (no subrect masking).
    cp.InWidth             = renderW;
    cp.InHeight            = renderH;
    cp.InTargetWidth       = outputW;
    cp.InTargetHeight      = outputH;
    cp.InPerfQualityValue  = static_cast<NVSDK_NGX_PerfQuality_Value>(ngxQuality);
    cp.InEnableOutputSubrects = false;
    // IsHDR: scene-linear input.  MVLowRes: MV at render res.  No AutoExposure (RR ignores it).
    cp.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR    | // DLSS §3.6 — scene-linear HDR input
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;   // DLSS §3.6.2 — MV at render res

    auto cmdPool = m_ctx->createComputeCommandPool();
    VkCommandBuffer initCmd = m_ctx->beginOneTimeCommands(cmdPool);

    // nvsdk_ngx_helpers_dlssd_vk.h — NGX_VULKAN_CREATE_DLSSD_EXT creates Feature_RayReconstruction (id 13).
    r = NGX_VULKAN_CREATE_DLSSD_EXT( // DLSS-RR §create
        initCmd, 1, 1, &m_handle, m_ngxParams, &cp);

    m_ctx->endOneTimeCommands(cmdPool, initCmd, m_ctx->computeQueue());
    vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);

    if (NVSDK_NGX_FAILED(r) || !m_handle) {
        throw std::runtime_error("NGX_VULKAN_CREATE_DLSSD_EXT failed: " + std::to_string(r));
    }

    LOG_INFO("[DLSS RR] Created RayReconstruction optimal={}x{} actual={}x{} "
             "range={}x{}..{}x{} -> {}x{} quality={} preset={} roughness={} "
             "IsHDR=1 MVLowRes=1 Depth=Linear Denoise=DLUnified EnableOutputSubrects=0",
             optW, optH, renderW, renderH,
             minW, minH, maxW, maxH, outputW, outputH,
             static_cast<int>(quality), static_cast<int>(preset),
             roughness == RrRoughnessMode::Packed ? "Packed" : "Unpacked");
}

// ──────────────────────────────────────────────────────────────────────────────
// evaluate()

void DlssRrPass::evaluate(VkCommandBuffer cmd, const RrInputs& in) {
    if (!m_handle) {
        throw std::runtime_error("DlssRrPass::evaluate called before create()");
    }

    // ── Validate mandatory buffers ────────────────────────────────────────
    if (in.colorRenderRes == VK_NULL_HANDLE || in.outputRes == VK_NULL_HANDLE ||
        in.depthLinear   == VK_NULL_HANDLE || in.motionVectors == VK_NULL_HANDLE ||
        in.diffuseAlbedo == VK_NULL_HANDLE || in.specularAlbedo == VK_NULL_HANDLE ||
        in.normals       == VK_NULL_HANDLE) {
        throw std::runtime_error("DlssRrPass::evaluate: missing mandatory image view");
    }
    if (m_roughnessMode == RrRoughnessMode::Unpacked && in.roughness == VK_NULL_HANDLE) {
        throw std::runtime_error("DlssRrPass::evaluate: Unpacked roughness mode requires roughness view");
    }
    if (m_roughnessMode == RrRoughnessMode::Packed && in.roughness != VK_NULL_HANDLE) {
        throw std::runtime_error("DlssRrPass::evaluate: Packed roughness must NOT pass roughness view (lives in normals.w)");
    }

    // ── Build NGX VK resource descriptors ────────────────────────────────
    auto rColor    = makeRrResource(in.colorRenderRes,   in.colorImage,          in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    auto rOutput   = makeRrResource(in.outputRes,        in.outputImage,         in.outputW, in.outputH, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    auto rDepth    = makeRrResource(in.depthLinear,      in.depthImage,          in.renderW, in.renderH, VK_FORMAT_R32_SFLOAT,          false);
    auto rMotion   = makeRrResource(in.motionVectors,    in.motionImage,         in.renderW, in.renderH, VK_FORMAT_R16G16_SFLOAT,       false);
    auto rDiffuse  = makeRrResource(in.diffuseAlbedo,    in.diffuseAlbedoImage,  in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    auto rSpecular = makeRrResource(in.specularAlbedo,   in.specularAlbedoImage, in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    auto rNormals  = makeRrResource(in.normals,          in.normalsImage,        in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, false);

    // Roughness: R32_SFLOAT when Unpacked; null when Packed (lives in normals.w).
    NVSDK_NGX_Resource_VK rRoughness{};
    bool haveRoughness = false;
    if (m_roughnessMode == RrRoughnessMode::Unpacked) {
        rRoughness = makeRrResource(in.roughness, in.roughnessImage, in.renderW, in.renderH, VK_FORMAT_R32_SFLOAT, false);
        haveRoughness = true;
    }

    // Specular motion (pInMotionVectorsReflection) — Blender 5.3+ optional.
    NVSDK_NGX_Resource_VK rSpecMotion{};
    bool haveSpecMotion = false;
    if (in.hasSpecularMotion &&
        in.specularMotion != VK_NULL_HANDLE &&
        in.specularMotionImage != VK_NULL_HANDLE) {
        rSpecMotion   = makeRrResource(in.specularMotion, in.specularMotionImage, in.renderW, in.renderH, VK_FORMAT_R16G16_SFLOAT, false);
        haveSpecMotion = true;
    }

    // ── Fill DLSSD eval params ────────────────────────────────────────────
    // nvsdk_ngx_helpers_dlssd_vk.h — NVSDK_NGX_VK_DLSSD_Eval_Params
    NVSDK_NGX_VK_DLSSD_Eval_Params ep{};

    // Core buffers (same contract as SR)
    ep.pInColor         = &rColor;          // DLSS-RR §inputs — noisy ray-traced color
    ep.pInOutput        = &rOutput;         // DLSS-RR §outputs — reconstructed output
    ep.pInDepth         = &rDepth;          // DLSS-RR §inputs — linear view-space depth
    ep.pInMotionVectors = &rMotion;         // DLSS-RR §inputs — camera+object motion

    // Guide buffers
    ep.pInDiffuseAlbedo  = &rDiffuse;       // DLSS-RR §inputs — Reinhard-clamped diffuse albedo
    ep.pInSpecularAlbedo = &rSpecular;      // DLSS-RR §inputs — raw specular albedo (no tonemap)
    ep.pInNormals        = &rNormals;       // DLSS-RR §inputs — world-space normalised normals
    ep.pInRoughness      = haveRoughness ? &rRoughness : nullptr; // DLSS-RR §inputs — null when Packed

    // Specular motion vectors (pInMotionVectorsReflection) — Blender 5.3+ Denoising Specular Motion
    ep.pInMotionVectorsReflection = haveSpecMotion ? &rSpecMotion : nullptr; // DLSS-RR §inputs

    // Jitter: pixel space, same sign as SR (from dlssconfig, computed by jitter.py)
    ep.InJitterOffsetX = in.jitterX;        // DLSS §3.7 — pixel space [-0.5, +0.5]
    ep.InJitterOffsetY = in.jitterY;        // DLSS §3.7

    // Subrect: full-frame, no subregion
    ep.InRenderSubrectDimensions = {in.renderW, in.renderH}; // DLSS §3.6

    // Motion vector scale: 1.0 — MVs are already pixel-unit from Blender.
    // DLSS_MV = (Blender.X, -Blender.Y); the sign flip is done in the EXR mapper (RrChannelMapper).
    ep.InMVScaleX = 1.0f;                   // DLSS SR §3.6.3 — pixel-unit MVs
    ep.InMVScaleY = 1.0f;

    // Exposure: RR does not support exposure scaling (docs §2); set to identity.
    ep.InPreExposure   = 1.0f;
    ep.InExposureScale = 1.0f;

    ep.InReset              = in.reset ? 1 : 0; // DLSS §3.13 — first frame/gap/cut only
    ep.InFrameTimeDeltaInMsec = in.frameTimeDeltaMs; // DLSS §3.14 — 1000/fps, never hardcoded

    // nvsdk_ngx_helpers_dlssd_vk.h — NGX_VULKAN_EVALUATE_DLSSD_EXT
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT( // DLSS-RR §evaluate
        cmd, m_handle, m_ngxParams, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        throw std::runtime_error("NGX_VULKAN_EVALUATE_DLSSD_EXT failed: " + std::to_string(r));
    }
}

// ──────────────────────────────────────────────────────────────────────────────

void DlssRrPass::destroy() {
    if (m_handle) {
        // DLSS SR §5.3 — Release the feature handle before destroying params/shutdown.
        (void)NVSDK_NGX_VULKAN_ReleaseFeature(m_handle); // DLSS §5.3
        m_handle = nullptr;
    }
    // Note: m_ngxParams is owned by NgxContext, not by this pass — do NOT destroy it here.
}

} // namespace dlss
