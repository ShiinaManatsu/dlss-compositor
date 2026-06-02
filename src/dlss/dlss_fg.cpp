#include "dlss_fg.h"
#include "../core/log.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_defs_dlssg.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace dlss {

namespace {

constexpr int kMinMultiplier = 2;
constexpr int kMaxMultiplier = 4;

void copyMatrix(float (&dst)[4][4], const float (&src)[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            dst[r][c] = src[r][c];
        }
    }
}

void setIdentity(float (&m)[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

NVSDK_NGX_Resource_VK makeImageViewResource(
    VkImageView view,
    VkImage image,
    VkImageAspectFlags aspect,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    bool readWrite) {
    const VkImageSubresourceRange range{
        aspect,
        0,
        1,
        0,
        1,
    };
    // DLSS FG §6 - Wrap Vulkan image view for NGX FG eval. // FG §6
    return NVSDK_NGX_Create_ImageView_Resource_VK( // DLSS §6; DLSS FG §6 // FG §6
        view, image, range, format, width, height, readWrite);
}

} // namespace

DlssFgPass::DlssFgPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool)
    : m_ngx(ngx), m_ctx(ctx), m_pool(pool)
{
    if (!m_ngx || !m_ctx) {
        throw std::invalid_argument("DlssFgPass requires valid NGX and Vulkan contexts");
    }
    m_ngxParams = m_ngx->params();
}

DlssFgPass::~DlssFgPass() {
    destroy();
}

void DlssFgPass::create(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, int multiplier) {
    if (multiplier < kMinMultiplier || multiplier > kMaxMultiplier) {
        throw std::invalid_argument("DLSS FG multiplier must be 2, 3, or 4");
    }

    if (m_handle) {
        destroy();
    }
    m_multiplier = multiplier;

    // DLSS FG §5 L459 - Set create parameters via NGX params. // FG §5 L459
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 L459 // FG §5 L459
        m_ngxParams,
        NVSDK_NGX_Parameter_Width,
        outW);
    // DLSS FG §5 L459 - Output/backbuffer dimensions. // FG §5 L459
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 L459 // FG §5 L459
        m_ngxParams,
        NVSDK_NGX_Parameter_Height,
        outH);
    // DLSS FG §5 L459 - DLSSG-specific output width. // FG §5 L459
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 L459 // FG §5 L459
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_Width,
        outW);
    // DLSS FG §5 L459 - DLSSG-specific output height. // FG §5 L459
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 L459 // FG §5 L459
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_Height,
        outH);

    // DLSS FG §5 - Render resolution (same dimensions as SR output input to FG). // FG §5
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 // FG §5
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_InternalWidth,
        inW);
    // DLSS FG §5 - Render resolution height. // FG §5
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 // FG §5
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_InternalHeight,
        inH);
    // DLSS FG §5 - Dynamic resolution disabled for fixed offline frames. // FG §5
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG §5 // FG §5
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_DynamicResolution,
        0u);

    // DLSS FG §HDR L1462 - Backbuffer format: RGB10A2 (PQ encoded for HDR). // FG §HDR L1462
    NVSDK_NGX_Parameter_SetUI( // DLSS §HDR; DLSS FG §HDR L1462 // FG §HDR L1462
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32);

    // DLSS FG §1 L518 - Multi-frame generation: multiplier-1 interp frames per input. // FG §1 L518
    NVSDK_NGX_Parameter_SetUI( // DLSS §1; DLSS FG §1 L518 // FG §1 L518
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
        static_cast<unsigned>(multiplier - 1));

    const uint32_t neverProvided =
        NVSDK_NGX_DLSSG_ResourceFlags_HUDLess |
        NVSDK_NGX_DLSSG_ResourceFlags_UI |
        NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
        NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
    // DLSS FG v310.6.0 - ResourceNeverProvided skips optional resources and saves VRAM. // FG §5
    NVSDK_NGX_Parameter_SetUI( // DLSS §5; DLSS FG v310.6.0 ResourceNeverProvided // FG §5
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
        neverProvided);

    auto pool = m_ctx->createComputeCommandPool();
    VkCommandBuffer initCmd = m_ctx->beginOneTimeCommands(pool);

    // DLSS FG §5 - Create the NGX Frame Generation feature. // FG §5
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_CreateFeature( // DLSS §5; DLSS FG §5 // FG §5
        initCmd,
        NVSDK_NGX_Feature_FrameGeneration,
        m_ngxParams,
        &m_handle);

    m_ctx->endOneTimeCommands(pool, initCmd, m_ctx->computeQueue());
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);

    if (NVSDK_NGX_FAILED(result)) { // DLSS §5; DLSS FG §5 // FG §5
        throw std::runtime_error("NVSDK_NGX_VULKAN_CreateFeature (FG) failed: " +
                                 std::to_string(result));
    }

    LOG_INFO("[DLSS FG] Created {}x{} -> {}x{} multiplier={}x", inW, inH, outW, outH, multiplier);
}

void DlssFgPass::evaluate(VkCommandBuffer cmd, const FgInputs& in) {
    if (!m_handle) {
        throw std::runtime_error("DlssFgPass::evaluate called before create()");
    }

    const int requestedMultiplier =
        (in.multiplier >= kMinMultiplier && in.multiplier <= kMaxMultiplier) ? in.multiplier : m_multiplier;
    const int frameCount = std::clamp(requestedMultiplier, kMinMultiplier, kMaxMultiplier) - 1;
    if (in.outputInterp.size() < static_cast<size_t>(frameCount)) {
        throw std::invalid_argument("DLSS FG outputInterp must contain multiplier-1 image views");
    }

    // DLSS FG §1 L518 - Multi-frame loop: eval (multiplier-1) times per input frame. // FG §1 L518
    for (int i = 0; i < frameCount; ++i) {
        NVSDK_NGX_VK_DLSSG_Eval_Params evalParams = {};
        NVSDK_NGX_DLSSG_Opt_Eval_Params optParams = {};
        const uint32_t depthMotionWidth = (in.depthMotionWidth != 0) ? in.depthMotionWidth : in.width;
        const uint32_t depthMotionHeight = (in.depthMotionHeight != 0) ? in.depthMotionHeight : in.height;
        auto backbufferResource = makeImageViewResource(
            in.backbufferDisplayReady,
            in.backbufferImage,
            VK_IMAGE_ASPECT_COLOR_BIT,
            in.backbufferFormat,
            in.width,
            in.height,
            false);
        auto depthResource = makeImageViewResource(
            in.depthNdcRenderRes,
            in.depthImage,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            in.depthFormat,
            depthMotionWidth,
            depthMotionHeight,
            false);
        auto motionVectorResource = makeImageViewResource(
            in.motionVecDlssRenderRes,
            in.motionVecImage,
            VK_IMAGE_ASPECT_COLOR_BIT,
            in.motionVecFormat,
            depthMotionWidth,
            depthMotionHeight,
            false);
        auto outputInterpResource = makeImageViewResource(
            in.outputInterp[static_cast<size_t>(i)],
            (in.outputInterpImages.size() > static_cast<size_t>(i)) ?
                in.outputInterpImages[static_cast<size_t>(i)] : VK_NULL_HANDLE,
            VK_IMAGE_ASPECT_COLOR_BIT,
            in.backbufferFormat,
            in.width,
            in.height,
            true);

        // DLSS §6; DLSS FG §6 L539 - Backbuffer is post-tonemap+EOTF display-ready. // FG §6 L539
        evalParams.pBackbuffer = &backbufferResource; // DLSS §6; DLSS FG §6 L539 // FG §6 L539

        // DLSS §6; DLSS FG §6 L545 - Depth: SAME image as SR (shared render-res depth). // FG §6 L545
        evalParams.pDepth = &depthResource; // DLSS §6; DLSS FG §6 L545 // FG §6 L545

        // DLSS §6; DLSS FG §6 L555 - Motion vectors: SAME image as SR. // FG §6 L555
        evalParams.pMVecs = &motionVectorResource; // DLSS §6; DLSS FG §6 L555 // FG §6 L555

        // DLSS §6; DLSS FG §6 - Output interpolated frame. // FG §6
        evalParams.pOutputInterpFrame = &outputInterpResource; // DLSS §6; DLSS FG §6 // FG §6

        // DLSS FG v310.6.0 - Optional resources are nullptr (ResourceNeverProvided set in create). // FG §6
        evalParams.pHudless = nullptr;
        evalParams.pUI = nullptr;
        evalParams.pUIAlpha = nullptr;
        evalParams.pBidirectionalDistortionField = nullptr;
        evalParams.pOutputRealFrame = nullptr; // DLSS FG §6 - offline: not needed (live game feature). // FG §6
        evalParams.pOutputDisableInterpolation = nullptr; // DLSS FG §6 - optional debug output. // FG §6

        optParams.multiFrameCount = static_cast<unsigned>(frameCount);
        // DLSS FG §1 L518 - SDK indices are 1..multiFrameCount. // FG §1 L518
        optParams.multiFrameIndex = static_cast<unsigned>(i + 1);

        // DLSS FG §6 L595 - Camera matrices from dlssconfig per-frame data. // FG §6 L595
        copyMatrix(optParams.cameraViewToClip, in.viewToClip);
        setIdentity(optParams.clipToCameraView);
        setIdentity(optParams.clipToLensClip);
        copyMatrix(optParams.clipToPrevClip, in.clipToPrevClip);
        copyMatrix(optParams.prevClipToClip, in.prevClipToClip);

        // DLSS FG §6 L591-593 - MvecScaleX = 1.0f and Y = 1.0f for pixel-unit MVs. // FG §6 L591
        optParams.mvecScale[0] = 1.0f;
        optParams.mvecScale[1] = 1.0f;
        optParams.jitterOffset[0] = 0.0f;
        optParams.jitterOffset[1] = 0.0f;
        optParams.cameraPinholeOffset[0] = 0.0f;
        optParams.cameraPinholeOffset[1] = 0.0f;

        // DLSS FG §6 L599 - Camera vectors and projection parameters. // FG §6 L599
        optParams.cameraPos[0] = in.camPosX;
        optParams.cameraPos[1] = in.camPosY;
        optParams.cameraPos[2] = in.camPosZ;
        optParams.cameraUp[0] = in.camUpX;
        optParams.cameraUp[1] = in.camUpY;
        optParams.cameraUp[2] = in.camUpZ;
        optParams.cameraRight[0] = in.camRightX;
        optParams.cameraRight[1] = in.camRightY;
        optParams.cameraRight[2] = in.camRightZ;
        optParams.cameraFwd[0] = in.camFwdX;
        optParams.cameraFwd[1] = in.camFwdY;
        optParams.cameraFwd[2] = in.camFwdZ;
        optParams.cameraNear = in.camNear;
        optParams.cameraFar = in.camFar;
        optParams.cameraFOV = in.fov;
        optParams.cameraAspectRatio = in.aspectRatio;

        // DLSS FG §HDR L1506 - HDR mode (1 = PQ encoded backbuffer). // FG §HDR L1506
        optParams.colorBuffersHDR = in.is_hdr;
        // DLSS FG §6 L590 - Blender NDC is non-inverted (0=near, 1=far). // FG §6 L590
        optParams.depthInverted = false;
        // DLSS FG §6 - MV includes camera motion and does not include jitter. // FG §6
        optParams.cameraMotionIncluded = true;
        optParams.motionVectorsDilated = false;
        optParams.menuDetectionEnabled = false;
        optParams.reset = in.reset;
        optParams.automodeOverrideReset = false;
        optParams.notRenderingGameFrames = false;
        optParams.orthoProjection = false;
        optParams.motionVectorsInvalidValue = 0.0f;
        optParams.minRelativeLinearDepthObjectSeparation = 40.0f;

        // DLSS FG §6 - Execute frame generation eval. // FG §6
        NVSDK_NGX_Result result = NGX_VK_EVALUATE_DLSSG( // DLSS §6; DLSS FG §6 // FG §6
            cmd,
            m_handle,
            m_ngxParams,
            &evalParams,
            &optParams);

        if (NVSDK_NGX_FAILED(result)) { // DLSS §6; DLSS FG §6 // FG §6
            LOG_ERROR("[DLSS FG] Eval failed at frame {}: {}", i, static_cast<int>(result));
            throw std::runtime_error("NGX_VK_EVALUATE_DLSSG failed at frame " + std::to_string(i));
        }
    }
}

void DlssFgPass::destroy() {
    if (m_handle) {
        // DLSS FG §5.3 - Release feature. // FG §5.3
        NVSDK_NGX_VULKAN_ReleaseFeature(m_handle); // DLSS §5.3; DLSS FG §5.3 // FG §5.3
        m_handle = nullptr;
    }
}

} // namespace dlss
