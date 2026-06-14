#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>
#include "../vulkan_context.h"
#include "../texture_pool.h"
#include "tonemap_pass.h"

namespace dlss {

/// Stage 6: Converts display-ready FG output back to scene-linear for EXR writing.
/// Symmetric inverse of Stage 4 TonemapPass.
/// Called ONLY in FG pipeline (SR-only path skips entirely).
///
/// LDR mode: inverse sRGB EOTF → linear [0,1].
/// HDR mode: inverse PQ ST.2084 → inverse ACES Filmic → scene-linear.
class InverseTonemapPass {
public:
    explicit InverseTonemapPass(VulkanContext* ctx);
    ~InverseTonemapPass();

    // Non-copyable, non-movable
    InverseTonemapPass(const InverseTonemapPass&)            = delete;
    InverseTonemapPass& operator=(const InverseTonemapPass&) = delete;

    /// Record inverse tonemap + inverse EOTF dispatch into command buffer.
    /// @param cmd             Active compute command buffer.
    /// @param srcDisplay      Source display-ready texture in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    /// @param dstSceneLinear  Destination storage image in VK_IMAGE_LAYOUT_GENERAL (scene-linear).
    /// @param mode            LDR = inverse sRGB only; HDR = inverse PQ + inverse ACES Filmic.
    void execute(VkCommandBuffer      cmd,
                 PooledTextureHandle  srcDisplay,
                 PooledTextureHandle  dstSceneLinear,
                 TonemapMode          mode);

private:
    void createDescriptorSetLayout();
    void createPipeline();

    VulkanContext*        m_ctx             = nullptr;
    VkShaderModule        m_shaderModule    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool        = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet         = VK_NULL_HANDLE;
    VkSampler             m_sampler         = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;
};

} // namespace dlss
