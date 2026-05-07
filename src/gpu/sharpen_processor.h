#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <string>

class VulkanContext;

/// Post-DLSS sharpening pass using an unsharp mask compute shader.
///
/// Operates on RGBA16F images in GENERAL layout.
/// sharpness = 0.0 is a bypass — caller should skip this pass entirely.
class SharpenProcessor {
public:
    explicit SharpenProcessor(VulkanContext& ctx);
    ~SharpenProcessor();

    /// Create the compute pipeline from pre-compiled SPIR-V.
    bool createPipeline(const std::string& spvPath, std::string& errorMsg);

    /// Record commands to apply sharpening.
    /// Both images must be in GENERAL layout.
    void cmdSharpen(VkCommandBuffer cmdBuf,
                    VkImage inputImage,
                    VkImageView inputView,
                    VkImage outputImage,
                    VkImageView outputView,
                    int width,
                    int height,
                    float sharpness);

    /// Release all Vulkan resources.
    void destroy();

    /// Check if pipeline is ready.
    bool isReady() const;

private:
    VulkanContext& m_ctx;

    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    bool m_pipelineCreated = false;

    void destroyPipeline();
};
