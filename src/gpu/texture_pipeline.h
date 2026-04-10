#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <cstdint>
#include <vector>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

struct TextureHandle {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0;
    int height = 0;
    int channels = 0;
};

class VulkanContext;

class TexturePipeline {
public:
    explicit TexturePipeline(VulkanContext& ctx);
    ~TexturePipeline();

    TextureHandle upload(const float* data, int width, int height, int channels, VkFormat format);
    std::vector<float> download(const TextureHandle& handle);
    void destroy(TextureHandle& handle);

private:
    VulkanContext& m_ctx;

    VkCommandBuffer beginOneTimeCommands();
    void endOneTimeCommands(VkCommandBuffer cmdBuf);
    void transitionImageLayout(VkCommandBuffer cmdBuf,
                               VkImage image,
                               VkImageLayout oldLayout,
                               VkImageLayout newLayout);
    uint16_t floatToHalf(float f);
    float halfToFloat(uint16_t h);
};
