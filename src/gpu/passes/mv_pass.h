#pragma once
#include "../vulkan_context.h"
#include "../texture_pool.h"

namespace dlss {

/// Converts Blender 4-channel Vector pass to DLSS 2-channel motion vectors.
/// Mapping: DLSS_MV = (Blender.X, -Blender.Y)
/// Per DLSS SR §3.6 + empirical verification in blender/tests/MV_SANITY_RESULT.md
class MvConversionPass {
public:
    MvConversionPass(VulkanContext* ctx);
    ~MvConversionPass();

    /// Record MV conversion dispatch into command buffer.
    /// @param srcBlender4ch  4-channel Blender Vector pass (R16G16B16A16_SFLOAT)
    /// @param dstDlss2ch     2-channel DLSS MV output (RG16F)
    void execute(VkCommandBuffer cmd,
                 PooledTextureHandle srcBlender4ch,
                 PooledTextureHandle dstDlss2ch);

private:
    void createPipeline();
    void createDescriptorSetLayout();

    VulkanContext*        m_ctx;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout  = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool        = VK_NULL_HANDLE;
    VkShaderModule        m_shaderModule    = VK_NULL_HANDLE;
    VkSampler             m_sampler         = VK_NULL_HANDLE;
};

} // namespace dlss
