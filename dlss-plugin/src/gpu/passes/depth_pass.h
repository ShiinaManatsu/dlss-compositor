#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>
#include "../vulkan_context.h"
#include "../texture_pool.h"

namespace dlss {

/// Reference CPU implementation of the Blender positive view-depth to hardware
/// depth conversion used by depth_world_to_ndc.comp.
float blenderViewDepthToNdc(float viewDepth,
                            const float projection[4][4]);

/// Converts Blender's positive camera/view-space Z pass to hardware depth [0,1].
/// Per DLSS SR §3.8: DLSS expects hardware/NDC depth, non-inverted.
///
/// Blender camera looks down -Z in view space, while its Z pass stores the
/// corresponding positive physical depth in scene units.
class DepthConversionPass {
public:
    explicit DepthConversionPass(VulkanContext* ctx);
    ~DepthConversionPass();

    // Non-copyable, non-movable
    DepthConversionPass(const DepthConversionPass&)            = delete;
    DepthConversionPass& operator=(const DepthConversionPass&) = delete;

    /// Record depth conversion dispatch into command buffer.
    /// @param cmd          Active compute command buffer to record into.
    /// @param srcCameraDistance  Source texture in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    /// @param dstNdc       Destination storage image in VK_IMAGE_LAYOUT_GENERAL.
    /// @param projection   Row-major Blender 4x4 projection matrix from dlssconfig.
    /// @param nearClip     Camera near clip plane (from dlssconfig).
    /// @param farClip      Camera far clip plane (from dlssconfig).
    void execute(VkCommandBuffer       cmd,
                 PooledTextureHandle   srcCameraDistance,
                 PooledTextureHandle   dstNdc,
                 const float           projection[4][4],
                 float                 nearClip,
                 float                 farClip);

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
