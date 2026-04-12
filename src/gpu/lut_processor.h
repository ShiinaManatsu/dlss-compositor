#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <cstdint>
#include <string>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

class VulkanContext;

/// Holds a loaded 3D LUT and the Vulkan compute pipeline to apply it.
class LutProcessor {
public:
    explicit LutProcessor(VulkanContext& ctx);
    ~LutProcessor();

    /// Load a raw float32 RGBA binary LUT file into a 3D VkImage.
    /// @param filepath  Path to the binary file (size^3 * 4 * sizeof(float) bytes).
    /// @param lutSize   Resolution per axis (e.g. 128).
    /// @param errorMsg  Error description on failure.
    /// @return true on success.
    bool loadLut(const std::string& filepath, int lutSize, std::string& errorMsg);

    /// Create the compute pipeline from pre-compiled SPIR-V.
    /// @param spvPath  Path to the compiled apply_lut.comp.spv file.
    /// @param errorMsg Error description on failure.
    /// @return true on success.
    bool createPipeline(const std::string& spvPath, std::string& errorMsg);

    /// Record commands to apply the loaded LUT to an image.
    /// The input image must be in GENERAL layout. The output image must be in GENERAL layout.
    /// After the dispatch, both images remain in GENERAL layout.
    ///
    /// @param cmdBuf         Active command buffer (recording state).
    /// @param inputImage     Source image (must be GENERAL layout).
    /// @param outputImage    Destination image (must be GENERAL layout).
    /// @param width          Image width in pixels.
    /// @param height         Image height in pixels.
    /// @param useLogShaper   true = scene-linear input (forward LUT, log2 shaper);
    ///                       false = display-referred input (inverse LUT, direct [0,1] coords).
    void apply(VkCommandBuffer cmdBuf,
               VkImage inputImage,
               VkImageView inputView,
               VkImage outputImage,
               VkImageView outputView,
               int width,
               int height,
               bool useLogShaper = true);

    /// Release all Vulkan resources.
    void destroy();

    /// Check if a LUT is loaded and pipeline is ready.
    bool isReady() const;

    /// Shaper parameters (must match Python generation script).
    static constexpr float kShaperLog2Min = -12.0f;
    static constexpr float kShaperLog2Max = 16.0f;

private:
    VulkanContext& m_ctx;

    // 3D LUT texture
    VkImage m_lutImage = VK_NULL_HANDLE;
    VkImageView m_lutView = VK_NULL_HANDLE;
    VmaAllocation m_lutAllocation = nullptr;
    VkSampler m_lutSampler = VK_NULL_HANDLE;
    int m_lutSize = 0;

    // Compute pipeline
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    bool m_lutLoaded = false;
    bool m_pipelineCreated = false;

    void destroyLut();
    void destroyPipeline();
};
