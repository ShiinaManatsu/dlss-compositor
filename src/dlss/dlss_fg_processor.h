#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "core/camera_data_loader.h"

#include <string>

class VulkanContext;
class NgxContext;

struct DlssFgFrameInput {
    // Current frame color buffer (DLSS-SR output or raw EXR color)
    VkImage backbuffer = VK_NULL_HANDLE;
    VkImageView backbufferView = VK_NULL_HANDLE;
    // Current frame depth
    VkImage depth = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    // Current frame motion vectors
    VkImage motionVectors = VK_NULL_HANDLE;
    VkImageView motionView = VK_NULL_HANDLE;
    // Output: interpolated frame written here
    VkImage outputInterp = VK_NULL_HANDLE;
    VkImageView outputInterpView = VK_NULL_HANDLE;
    // Frame dimensions
    uint32_t width = 0;
    uint32_t height = 0;
    // true for first frame or after sequence gap
    bool reset = false;
    // Camera parameters derived from CameraDataLoader::computePairParams()
    DlssFgCameraParams cameraParams{};
    // 1 for 2x interpolation (one intermediate frame), 3 for 4x (three intermediate frames)
    unsigned int multiFrameCount = 1;
    // 1-based index of this interpolated frame (1..multiFrameCount)
    unsigned int multiFrameIndex = 1;
};

class DlssFgProcessor {
public:
    DlssFgProcessor(VulkanContext& ctx, NgxContext& ngx);

    bool evaluate(VkCommandBuffer cmdBuf, const DlssFgFrameInput& frame, std::string& errorMsg);

private:
    VulkanContext& m_ctx;
    NgxContext& m_ngx;
};
