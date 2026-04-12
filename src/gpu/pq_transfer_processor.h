#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <string>

class VulkanContext;

/// Per-channel PQ (ST 2084) encode/decode for HDR transport around Frame Generation.
///
/// Unlike LutProcessor, this uses pure shader math — no 3D LUT texture, no lookup
/// artifacts, perfect roundtrip. Scene-linear 1.0 maps to 100 nits (sRGB reference
/// white), with a max of 10 000 nits.
class PqTransferProcessor {
public:
    explicit PqTransferProcessor(VulkanContext& ctx);
    ~PqTransferProcessor();

    /// Create the compute pipeline from pre-compiled SPIR-V.
    /// @param spvPath  Path to pq_transfer.comp.spv.
    /// @param errorMsg Error description on failure.
    /// @return true on success.
    bool createPipeline(const std::string& spvPath, std::string& errorMsg);

    /// Record commands to encode (scene-linear → PQ) or decode (PQ → scene-linear).
    /// Both images must be in GENERAL layout.
    ///
    /// @param cmdBuf      Active command buffer.
    /// @param inputImage  Source image (GENERAL layout).
    /// @param inputView   Source image view.
    /// @param outputImage Destination image (GENERAL layout).
    /// @param outputView  Destination image view.
    /// @param width       Image width in pixels.
    /// @param height      Image height in pixels.
    /// @param encode      true = scene-linear→PQ, false = PQ→scene-linear.
    void apply(VkCommandBuffer cmdBuf,
               VkImage inputImage,
               VkImageView inputView,
               VkImage outputImage,
               VkImageView outputView,
               int width,
               int height,
               bool encode);

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
