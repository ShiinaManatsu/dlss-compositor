#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <string>

class VulkanContext;
class NgxContext;

struct DlssSRFrameInput {
    VkImage color = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkImage depth = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImage motionVectors = VK_NULL_HANDLE;
    VkImageView motionView = VK_NULL_HANDLE;
    // Optional GBuffer hints — VK_NULL_HANDLE to skip
    VkImage diffuseAlbedo = VK_NULL_HANDLE;
    VkImageView diffuseView = VK_NULL_HANDLE;
    VkImage normals = VK_NULL_HANDLE;
    VkImageView normalsView = VK_NULL_HANDLE;
    VkImage roughness = VK_NULL_HANDLE;
    VkImageView roughnessView = VK_NULL_HANDLE;
    VkImage output = VK_NULL_HANDLE;
    VkImageView outputView = VK_NULL_HANDLE;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    bool reset = false;
};

class DlssSRProcessor {
public:
    DlssSRProcessor(VulkanContext& ctx, NgxContext& ngx);

    bool evaluate(VkCommandBuffer cmdBuf, const DlssSRFrameInput& frame, std::string& errorMsg);

private:
    VulkanContext& m_ctx;
    NgxContext& m_ngx;
};
