#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "dlss/dlss_fg_processor.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/vulkan_context.h"

#include <nvsdk_ngx_helpers_dlssg_vk.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <cstring>

namespace {

std::string ngxResultToString(NVSDK_NGX_Result result) {
    const wchar_t* message = GetNGXResultAsString(result);
    if (message == nullptr) {
        return "unknown NGX error";
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return "unknown NGX error";
    }

    std::string text(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), size, nullptr, nullptr);
    return text;
}

NVSDK_NGX_Resource_VK makeImageResource(VkImage image,
                                        VkImageView view,
                                        VkFormat format,
                                        uint32_t width,
                                        uint32_t height,
                                        bool readWrite) {
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    NVSDK_NGX_Resource_VK resource{};
    resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    resource.Resource.ImageViewInfo.ImageView = view;
    resource.Resource.ImageViewInfo.Image = image;
    resource.Resource.ImageViewInfo.SubresourceRange = range;
    resource.Resource.ImageViewInfo.Format = format;
    resource.Resource.ImageViewInfo.Width = width;
    resource.Resource.ImageViewInfo.Height = height;
    resource.ReadWrite = readWrite;
    return resource;
}

} // namespace

DlssFgProcessor::DlssFgProcessor(VulkanContext& ctx, NgxContext& ngx) : m_ctx(ctx), m_ngx(ngx) {}

bool DlssFgProcessor::evaluate(VkCommandBuffer cmdBuf, const DlssFgFrameInput& frame, std::string& errorMsg) {
    errorMsg.clear();

    if (!m_ctx.isInitialized()) {
        errorMsg = "Vulkan context is not initialized.";
        return false;
    }
    if (!m_ngx.isInitialized()) {
        errorMsg = "NGX context is not initialized.";
        return false;
    }
    if (m_ngx.fgFeatureHandle() == nullptr) {
        errorMsg = "DLSS-G feature handle is not created.";
        return false;
    }
    if (m_ngx.parameters() == nullptr) {
        errorMsg = "NGX parameters are unavailable.";
        return false;
    }
    if (cmdBuf == VK_NULL_HANDLE) {
        errorMsg = "Command buffer is null.";
        return false;
    }
    if (frame.width == 0 || frame.height == 0) {
        errorMsg = "Frame dimensions must be non-zero.";
        return false;
    }
    if (frame.backbuffer == VK_NULL_HANDLE) {
        errorMsg = "Backbuffer image is null.";
        return false;
    }
    if (frame.backbufferView == VK_NULL_HANDLE) {
        errorMsg = "Backbuffer image view is null.";
        return false;
    }
    if (frame.depth == VK_NULL_HANDLE) {
        errorMsg = "Depth image is null.";
        return false;
    }
    if (frame.depthView == VK_NULL_HANDLE) {
        errorMsg = "Depth image view is null.";
        return false;
    }
    if (frame.motionVectors == VK_NULL_HANDLE) {
        errorMsg = "Motion vectors image is null.";
        return false;
    }
    if (frame.motionView == VK_NULL_HANDLE) {
        errorMsg = "Motion vectors image view is null.";
        return false;
    }
    if (frame.outputInterp == VK_NULL_HANDLE) {
        errorMsg = "Output interpolation image is null.";
        return false;
    }
    if (frame.outputInterpView == VK_NULL_HANDLE) {
        errorMsg = "Output interpolation image view is null.";
        return false;
    }

    NVSDK_NGX_Resource_VK backbufferResource = makeImageResource(
        frame.backbuffer, frame.backbufferView, VK_FORMAT_R16G16B16A16_SFLOAT, frame.width, frame.height, false);
    NVSDK_NGX_Resource_VK depthResource = makeImageResource(
        frame.depth, frame.depthView, VK_FORMAT_R32_SFLOAT, frame.width, frame.height, false);
    NVSDK_NGX_Resource_VK motionResource = makeImageResource(
        frame.motionVectors, frame.motionView, VK_FORMAT_R16G16_SFLOAT, frame.width, frame.height, false);
    NVSDK_NGX_Resource_VK outputResource = makeImageResource(
        frame.outputInterp, frame.outputInterpView, VK_FORMAT_R16G16B16A16_SFLOAT, frame.width, frame.height, true);

    NVSDK_NGX_VK_DLSSG_Eval_Params evalParams{};
    evalParams.pBackbuffer = &backbufferResource;
    evalParams.pDepth = &depthResource;
    evalParams.pMVecs = &motionResource;
    evalParams.pHudless = nullptr;
    evalParams.pUI = nullptr;
    evalParams.pNoPostProcessingColor = nullptr;
    evalParams.pBidirectionalDistortionField = nullptr;
    evalParams.pOutputInterpFrame = &outputResource;
    evalParams.pOutputRealFrame = nullptr;
    evalParams.pOutputDisableInterpolation = nullptr;

    NVSDK_NGX_DLSSG_Opt_Eval_Params optEvalParams{};
    optEvalParams.multiFrameCount = frame.multiFrameCount;
    optEvalParams.multiFrameIndex = frame.multiFrameIndex;
    std::memcpy(optEvalParams.cameraViewToClip, frame.cameraParams.cameraViewToClip, sizeof(float) * 16);
    std::memcpy(optEvalParams.clipToCameraView, frame.cameraParams.clipToCameraView, sizeof(float) * 16);
    std::memcpy(optEvalParams.clipToLensClip, frame.cameraParams.clipToLensClip, sizeof(float) * 16);
    std::memcpy(optEvalParams.clipToPrevClip, frame.cameraParams.clipToPrevClip, sizeof(float) * 16);
    std::memcpy(optEvalParams.prevClipToClip, frame.cameraParams.prevClipToClip, sizeof(float) * 16);
    optEvalParams.mvecScale[0] = 1.0f / static_cast<float>(frame.width);
    optEvalParams.mvecScale[1] = 1.0f / static_cast<float>(frame.height);
    optEvalParams.jitterOffset[0] = 0.0f;
    optEvalParams.jitterOffset[1] = 0.0f;
    optEvalParams.cameraPinholeOffset[0] = 0.0f;
    optEvalParams.cameraPinholeOffset[1] = 0.0f;
    optEvalParams.cameraPos[0] = frame.cameraParams.cameraPos[0];
    optEvalParams.cameraPos[1] = frame.cameraParams.cameraPos[1];
    optEvalParams.cameraPos[2] = frame.cameraParams.cameraPos[2];
    optEvalParams.cameraUp[0] = frame.cameraParams.cameraUp[0];
    optEvalParams.cameraUp[1] = frame.cameraParams.cameraUp[1];
    optEvalParams.cameraUp[2] = frame.cameraParams.cameraUp[2];
    optEvalParams.cameraRight[0] = frame.cameraParams.cameraRight[0];
    optEvalParams.cameraRight[1] = frame.cameraParams.cameraRight[1];
    optEvalParams.cameraRight[2] = frame.cameraParams.cameraRight[2];
    optEvalParams.cameraFwd[0] = frame.cameraParams.cameraForward[0];
    optEvalParams.cameraFwd[1] = frame.cameraParams.cameraForward[1];
    optEvalParams.cameraFwd[2] = frame.cameraParams.cameraForward[2];
    optEvalParams.cameraNear = frame.cameraParams.nearPlane;
    optEvalParams.cameraFar = frame.cameraParams.farPlane;
    optEvalParams.cameraFOV = frame.cameraParams.fov;
    optEvalParams.cameraAspectRatio = frame.cameraParams.aspectRatio;
    optEvalParams.colorBuffersHDR = true;
    optEvalParams.depthInverted = false;
    optEvalParams.cameraMotionIncluded = true;
    optEvalParams.reset = frame.reset;
    optEvalParams.automodeOverrideReset = false;
    optEvalParams.notRenderingGameFrames = false;
    optEvalParams.orthoProjection = false;
    optEvalParams.motionVectorsInvalidValue = 0.0f;
    optEvalParams.motionVectorsDilated = false;
    optEvalParams.menuDetectionEnabled = false;

    const NVSDK_NGX_Result result = NGX_VK_EVALUATE_DLSSG(
        cmdBuf,
        m_ngx.fgFeatureHandle(),
        m_ngx.parameters(),
        &evalParams,
        &optEvalParams);
    if (NVSDK_NGX_FAILED(result)) {
        errorMsg = "DLSS-G evaluate failed: " + ngxResultToString(result);
        return false;
    }

    return true;
}
