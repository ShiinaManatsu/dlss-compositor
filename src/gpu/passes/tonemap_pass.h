#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>
#include "../vulkan_context.h"
#include "../texture_pool.h"

namespace dlss {

enum class TonemapMode {
    LDR = 0,  // sRGB EOTF only (no tonemap) — for LDR projects
    HDR = 1   // ACES Filmic + PQ ST.2084 — for HDR projects
};

/// Stage 4: Converts scene-linear → display-ready (for FG input).
/// Called ONLY in FG pipeline (SR-only path skips this entirely).
/// Per DLSS FG §HDR: FG requires display-ready backbuffer input.
class TonemapPass {
public:
    explicit TonemapPass(VulkanContext* ctx);
    ~TonemapPass();

    // Non-copyable, non-movable
    TonemapPass(const TonemapPass&)            = delete;
    TonemapPass& operator=(const TonemapPass&) = delete;

    /// Record tonemap + EOTF dispatch into command buffer.
    /// @param cmd           Active compute command buffer.
    /// @param srcSceneLinear  Source HDR texture in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    /// @param dstDisplay    Destination storage image in VK_IMAGE_LAYOUT_GENERAL.
    /// @param mode          LDR = sRGB-only; HDR = ACES Filmic + PQ ST.2084.
    void execute(VkCommandBuffer      cmd,
                 PooledTextureHandle  srcSceneLinear,
                 PooledTextureHandle  dstDisplay,
                 TonemapMode          mode);

private:
    void createDescriptorSetLayout();
    void createPipeline();

    VulkanContext*        m_ctx             = nullptr;
    VkShaderModule        m_shaderModule    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;
};

} // namespace dlss
