#pragma once

#include "../texture_pool.h"
#include "../vulkan_context.h"

#include <cstdint>
#include <vector>

namespace dlss {

enum class PreviewInputMode {
    SceneLinear = 0,
    DisplayReady = 1,
};

struct PreviewReadback {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

class PreviewPass {
public:
    explicit PreviewPass(VulkanContext* ctx);
    ~PreviewPass();

    PreviewPass(const PreviewPass&) = delete;
    PreviewPass& operator=(const PreviewPass&) = delete;

    PreviewReadback renderAndReadback(PooledTextureHandle src,
                                      PreviewInputMode mode,
                                      bool hdr,
                                      std::uint32_t maxLongEdge = 512);

private:
    void createDescriptorSetLayout();
    void createPipeline();
    PooledTextureHandle acquirePreviewTexture(std::uint32_t srcWidth,
                                              std::uint32_t srcHeight,
                                              std::uint32_t maxLongEdge);
    void record(VkCommandBuffer cmd,
                PooledTextureHandle src,
                PooledTextureHandle dst,
                PreviewInputMode mode,
                bool hdr);

    VulkanContext* m_ctx = nullptr;
    TexturePool m_pool;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

} // namespace dlss
