#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "dlss/dlss_sr_processor.h"

#include "dlss/ngx_wrapper.h"
#include "gpu/vulkan_context.h"

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

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

bool validateImage(const char* name,
                   VkImage image,
                   VkImageView view,
                   uint32_t width,
                   uint32_t height,
                   std::string& errorMsg) {
    if (image == VK_NULL_HANDLE) {
        errorMsg = std::string(name) + " image is null.";
        return false;
    }
    if (view == VK_NULL_HANDLE) {
        errorMsg = std::string(name) + " image view is null.";
        return false;
    }
    if (width == 0 || height == 0) {
        errorMsg = std::string(name) + " dimensions must be non-zero.";
        return false;
    }
    return true;
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

DlssSRProcessor::DlssSRProcessor(VulkanContext& ctx, NgxContext& ngx) : m_ctx(ctx), m_ngx(ngx) {}

bool DlssSRProcessor::evaluate(VkCommandBuffer cmdBuf, const DlssSRFrameInput& frame, std::string& errorMsg) {
    errorMsg.clear();

    if (!m_ctx.isInitialized()) {
        errorMsg = "Vulkan context is not initialized.";
        return false;
    }
    if (!m_ngx.isInitialized()) {
        errorMsg = "NGX context is not initialized.";
        return false;
    }
    if (m_ngx.getFeatureHandle() == nullptr) {
        errorMsg = "DLSS-SR feature handle is not created.";
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

    if (!validateImage("color", frame.color, frame.colorView, frame.inputWidth, frame.inputHeight, errorMsg) ||
        !validateImage("depth", frame.depth, frame.depthView, frame.inputWidth, frame.inputHeight, errorMsg) ||
        !validateImage("motion vectors", frame.motionVectors, frame.motionView, frame.inputWidth, frame.inputHeight, errorMsg) ||
        !validateImage("output", frame.output, frame.outputView, frame.outputWidth, frame.outputHeight, errorMsg)) {
        return false;
    }

    if (frame.diffuseAlbedo != VK_NULL_HANDLE &&
        !validateImage("diffuse albedo", frame.diffuseAlbedo, frame.diffuseView, frame.inputWidth, frame.inputHeight, errorMsg)) {
        return false;
    }
    if (frame.normals != VK_NULL_HANDLE &&
        !validateImage("normals", frame.normals, frame.normalsView, frame.inputWidth, frame.inputHeight, errorMsg)) {
        return false;
    }
    if (frame.roughness != VK_NULL_HANDLE &&
        !validateImage("roughness", frame.roughness, frame.roughnessView, frame.inputWidth, frame.inputHeight, errorMsg)) {
        return false;
    }

    NVSDK_NGX_Resource_VK colorResource = makeImageResource(
        frame.color, frame.colorView, VK_FORMAT_R16G16B16A16_SFLOAT, frame.inputWidth, frame.inputHeight, false);
    NVSDK_NGX_Resource_VK depthResource = makeImageResource(
        frame.depth, frame.depthView, VK_FORMAT_R32_SFLOAT, frame.inputWidth, frame.inputHeight, false);
    NVSDK_NGX_Resource_VK motionResource = makeImageResource(
        frame.motionVectors, frame.motionView, VK_FORMAT_R16G16_SFLOAT, frame.inputWidth, frame.inputHeight, false);
    NVSDK_NGX_Resource_VK outputResource = makeImageResource(
        frame.output, frame.outputView, VK_FORMAT_R16G16B16A16_SFLOAT, frame.outputWidth, frame.outputHeight, true);
    NVSDK_NGX_Resource_VK albedoResource{};
    NVSDK_NGX_Resource_VK normalsResource{};
    NVSDK_NGX_Resource_VK roughnessResource{};

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams{};
    evalParams.Feature.pInColor = &colorResource;
    evalParams.Feature.pInOutput = &outputResource;
    evalParams.pInDepth = &depthResource;
    evalParams.pInMotionVectors = &motionResource;
    evalParams.InJitterOffsetX = frame.jitterX;
    evalParams.InJitterOffsetY = frame.jitterY;
    evalParams.InRenderSubrectDimensions.Width = frame.inputWidth;
    evalParams.InRenderSubrectDimensions.Height = frame.inputHeight;
    evalParams.InReset = frame.reset ? 1 : 0;
    evalParams.InMVScaleX = frame.mvScaleX;
    evalParams.InMVScaleY = frame.mvScaleY;
    evalParams.InFrameTimeDeltaInMsec = 33.3f;
    evalParams.InPreExposure = 1.0f;
    evalParams.InExposureScale = 1.0f;

    if (frame.diffuseAlbedo != VK_NULL_HANDLE) {
        albedoResource = makeImageResource(
            frame.diffuseAlbedo, frame.diffuseView, VK_FORMAT_R16G16B16A16_SFLOAT,
            frame.inputWidth, frame.inputHeight, false);
        evalParams.GBufferSurface.pInAttrib[NVSDK_NGX_GBUFFER_ALBEDO] = &albedoResource;
    }
    if (frame.normals != VK_NULL_HANDLE) {
        normalsResource = makeImageResource(
            frame.normals, frame.normalsView, VK_FORMAT_R16G16B16A16_SFLOAT,
            frame.inputWidth, frame.inputHeight, false);
        evalParams.GBufferSurface.pInAttrib[NVSDK_NGX_GBUFFER_NORMALS] = &normalsResource;
    }
    if (frame.roughness != VK_NULL_HANDLE) {
        roughnessResource = makeImageResource(
            frame.roughness, frame.roughnessView, VK_FORMAT_R32_SFLOAT,
            frame.inputWidth, frame.inputHeight, false);
        evalParams.GBufferSurface.pInAttrib[NVSDK_NGX_GBUFFER_ROUGHNESS] = &roughnessResource;
    }

    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(
        cmdBuf,
        m_ngx.getFeatureHandle(),
        m_ngx.parameters(),
        &evalParams);
    if (NVSDK_NGX_FAILED(result)) {
        errorMsg = "DLSS-SR evaluate failed: " + ngxResultToString(result);
        return false;
    }

    return true;
}
