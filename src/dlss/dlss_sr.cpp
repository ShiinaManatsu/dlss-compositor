#include "dlss_sr.h"
#include "../core/log.h"

// DLSS NGX headers (Vulkan path)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs.h>

#include <stdexcept>
#include <string>

namespace dlss {

namespace {

NVSDK_NGX_Resource_VK makeDlssResource(VkImageView imageView, uint32_t width, uint32_t height, VkFormat format, bool readWrite) {
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    return NVSDK_NGX_Create_ImageView_Resource_VK(
        imageView,
        VK_NULL_HANDLE,
        range,
        format,
        width,
        height,
        readWrite);
}

} // namespace

DlssSrPass::DlssSrPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool)
    : m_ngx(ngx), m_ctx(ctx), m_pool(pool)
{
    // Reuse the capability parameters from NgxContext.
    m_ngxParams = m_ngx->params();
}

DlssSrPass::~DlssSrPass() {
    destroy();
}

void DlssSrPass::create(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, QualityMode quality) {
    if (m_handle) destroy();

    // DLSS SR §3.5 - Feature creation parameters
    NVSDK_NGX_DLSS_Create_Params createParams = {};
    // DLSS SR §3.5 - Input render resolution (before upscaling)
    createParams.Feature.InWidth  = inW;
    createParams.Feature.InHeight = inH;
    // DLSS SR §3.5 - Output/target resolution (after upscaling)
    createParams.Feature.InTargetWidth  = outW;
    createParams.Feature.InTargetHeight = outH;
    // DLSS SR §3.8 - Quality/performance mode
    createParams.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(toNgxQuality(quality));
    // DLSS SR §3.6.2 - MVLowRes: MV provided at render resolution (not output res)
    // DLSS SR §3.10 - AutoExposure: let DLSS compute exposure automatically
    // NOTE: IsHDR=1 because inputs are scene-linear HDR (R16G16B16A16_SFLOAT)
    // NOTE: SR §3.11 sharpness control is deprecated; DLSS model produces sharp output natively
    createParams.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR        | // DLSS SR §3.6 - HDR scene-linear input
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes      | // DLSS SR §3.6.2 - MV at render res
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;   // DLSS SR §3.10 - auto exposure

    // DLSS SR §3.5 - Create the feature
    auto pool = m_ctx->createComputeCommandPool();
    VkCommandBuffer initCmd = m_ctx->beginOneTimeCommands(pool);

    NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT( // DLSS §3.5
        initCmd, 1, 1, &m_handle, m_ngxParams, &createParams);

    m_ctx->endOneTimeCommands(pool, initCmd, m_ctx->computeQueue());
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);

    if (NVSDK_NGX_FAILED(result)) { // DLSS §3.5 // DLSS §3.5
        throw std::runtime_error("NGX_VULKAN_CREATE_DLSS_EXT failed: " + std::to_string(result));
    }

    // DLSS SR §3.12 - Set per-quality preset (DLAA/Q/B/UQ→K, P→M, UP→L)
    setPresetForQuality(m_ngxParams, quality);

    LOG_INFO("[DLSS SR] Created {}x{} -> {}x{} quality={}", inW, inH, outW, outH, static_cast<int>(quality));
}

void DlssSrPass::evaluate(VkCommandBuffer cmd, const SrInputs& in) {
    if (!m_handle) {
        throw std::runtime_error("DlssSrPass::evaluate called before create()");
    }

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {};
    auto colorResource = makeDlssResource(
        in.sceneColorRenderRes, in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    auto outputResource = makeDlssResource(
        in.sceneColorOutputRes, in.renderW, in.renderH, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    auto depthResource = makeDlssResource(
        in.depthNdc, in.renderW, in.renderH, VK_FORMAT_R32_SFLOAT, false);
    auto motionVectorResource = makeDlssResource(
        in.motionVecDlss, in.renderW, in.renderH, VK_FORMAT_R16G16_SFLOAT, false);

    // DLSS §3.6 - Input color resource (scene-linear, at render resolution)
    evalParams.Feature.pInColor = &colorResource;
    // DLSS §3.6 - Output color resource (at target/output resolution)
    evalParams.Feature.pInOutput = &outputResource;

    // DLSS §3.5 - Depth resource in NDC [0,1], non-inverted
    evalParams.pInDepth = &depthResource;
    // DLSS §3.6 - Motion vector resource, curr→prev, pixel units
    evalParams.pInMotionVectors = &motionVectorResource;

    // DLSS SR §3.6 - Subrect bases (0,0 for full-frame, no subregion)
    evalParams.InColorSubrectBase    = {0, 0};  // DLSS SR §3.6
    evalParams.InDepthSubrectBase    = {0, 0};  // DLSS SR §3.6
    evalParams.InMVSubrectBase       = {0, 0};  // DLSS SR §3.6
    evalParams.InOutputSubrectBase   = {0, 0};  // DLSS SR §3.6
    evalParams.InRenderSubrectDimensions = {in.renderW, in.renderH};  // DLSS SR §3.6

    // DLSS SR §3.10 - AutoExposure: set pInExposureTexture=nullptr, InPreExposure=1.0
    evalParams.pInExposureTexture = nullptr;     // DLSS SR §3.10 - auto exposure
    evalParams.InPreExposure      = 1.0f;        // DLSS SR §3.10
    evalParams.InExposureScale    = 1.0f;        // DLSS SR §3.10

    // DLSS SR §3.7 - Jitter in pixel space [-0.5, +0.5]
    evalParams.InJitterOffsetX = in.jitterX;     // DLSS SR §3.7
    evalParams.InJitterOffsetY = in.jitterY;     // DLSS SR §3.7

    // DLSS SR §3.6.3 - MV scale: 1.0 because MVs are already in pixel units
    // IMPORTANT: Do NOT use (1/width, 1/height) — MVs from T23 are pixel-unit, not normalized
    evalParams.InMVScaleX = 1.0f;                // DLSS SR §3.6.3 - pixel-unit MVs
    evalParams.InMVScaleY = 1.0f;                // DLSS SR §3.6.3 - pixel-unit MVs

    // DLSS SR §3.13 - Reset on first frame / frame gap / camera cut
    // Never reset every frame — temporal history is the whole point of DLSS SR
    evalParams.InReset = in.reset ? 1 : 0;       // DLSS SR §3.13

    // DLSS SR §3.14 - Frame time delta in milliseconds (derived from fps, never hardcoded)
    evalParams.InFrameTimeDeltaInMsec = in.frameTimeDeltaMs;  // DLSS SR §3.14

    // DLSS SR §3.6 - Optional bias mask (nullptr = not used)
    evalParams.pInBiasCurrentColorMask = nullptr;  // DLSS SR §3.6 - optional

    // DLSS SR §3.6 - Execute SR evaluation
    NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT( // DLSS §3.6
        cmd, m_handle, m_ngxParams, &evalParams);

    if (NVSDK_NGX_FAILED(result)) { // DLSS §3.6 // DLSS §3.6
        LOG_ERROR("[DLSS SR] Eval failed: {}", static_cast<int>(result));
        throw std::runtime_error("NGX_VULKAN_EVALUATE_DLSS_EXT failed");
    }
}

void DlssSrPass::destroy() {
    if (m_handle) {
        // DLSS SR §5.3 - Release feature when done
        NVSDK_NGX_VULKAN_ReleaseFeature(m_handle); // DLSS §5.3
        m_handle = nullptr;
    }
}

int DlssSrPass::toNgxQuality(QualityMode q) {
    // DLSS §3.8 - Quality mode mapping
    switch (q) {
        case QualityMode::DLAA:             return NVSDK_NGX_PerfQuality_Value_DLAA;
        case QualityMode::UltraQuality:     return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case QualityMode::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case QualityMode::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
        case QualityMode::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case QualityMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default: return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

void DlssSrPass::setPresetForQuality(NVSDK_NGX_Parameter* params, QualityMode q) {
    // DLSS §3.12 - Per-quality preset selection
    // Recommended presets: DLAA/UltraQuality/Quality/Balanced → K; Performance → M; UltraPerformance → L
    NVSDK_NGX_DLSS_Hint_Render_Preset preset;
    switch (q) {
        case QualityMode::Performance:      preset = NVSDK_NGX_DLSS_Hint_Render_Preset_M; break;  // DLSS SR §3.12
        case QualityMode::UltraPerformance: preset = NVSDK_NGX_DLSS_Hint_Render_Preset_L; break;  // DLSS SR §3.12
        default:                            preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K; break;  // DLSS SR §3.12
    }

    // DLSS SR §3.12 - Apply preset hint via NGX parameters
    NVSDK_NGX_Parameter_SetUI( // DLSS §3.12
        params,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        static_cast<unsigned>(preset));
}

} // namespace dlss
