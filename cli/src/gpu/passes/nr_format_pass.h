#pragma once
// NrFormatPass — display-domain format conversion around the DLSSNR pass.
//
// DLSSNR (NGX feature 18) only accepts SDR RGBA8_UNORM in/out; the rest of the
// pipeline moves display-ready frames as A2B10G10R10_UNORM_PACK32. Two tiny
// compute shaders convert between them (pure bit-depth change, no color math):
//   pack   : rgb10_a2 → rgba8   (before NR)
//   unpack : rgba8 → rgb10_a2   (after NR)

#include "../texture_pool.h"

#include <volk.h>

#include <string>

namespace dlss {

class NrFormatPass {
public:
    /// mode "pack" loads nr_pack_rgba8.comp.spv; "unpack" loads nr_unpack_rgba8.comp.spv.
    NrFormatPass(VulkanContext* ctx, const char* mode);
    ~NrFormatPass();

    NrFormatPass(const NrFormatPass&) = delete;
    NrFormatPass& operator=(const NrFormatPass&) = delete;

    /// src must be in SHADER_READ_ONLY_OPTIMAL, dst transitions UNDEFINED → GENERAL.
    void execute(VkCommandBuffer cmd, PooledTextureHandle src, PooledTextureHandle dst);

private:
    void createDescriptorSetLayout();
    void createPipeline(const char* mode);

    VulkanContext*     m_ctx;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet       = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkShaderModule        m_shaderModule   = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;
};

} // namespace dlss
