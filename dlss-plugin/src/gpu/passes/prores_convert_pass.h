#pragma once

#include "../texture_pool.h"
#include "../vulkan_context.h"
#include "../../io/encoder_registry.h"

#include <cstdint>
#include <vector>

namespace dlss {

class ProresConvertPass {
public:
    explicit ProresConvertPass(VulkanContext* ctx);
    ~ProresConvertPass();

    ProresConvertPass(const ProresConvertPass&) = delete;
    ProresConvertPass& operator=(const ProresConvertPass&) = delete;

    std::vector<std::uint8_t> convertAndReadback(PooledTextureHandle src,
                                                 PixelEncoding pixelEncoding,
                                                 bool hdr,
                                                 VkImageLayout srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

private:
    void createDescriptorSetLayout();
    void createPipeline();
    void record(VkCommandBuffer cmd,
                PooledTextureHandle src,
                VkBuffer dstBuffer,
                std::uint32_t width,
                std::uint32_t height,
                PixelEncoding pixelEncoding,
                bool hdr,
                VkImageLayout srcLayout);

    VulkanContext* m_ctx = nullptr;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
};

} // namespace dlss
