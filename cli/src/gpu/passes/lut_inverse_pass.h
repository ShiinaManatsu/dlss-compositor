#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "../../core/lut_loader.h"
#include "../texture_pool.h"
#include "../vulkan_context.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace dlss {

class LutInversePass {
public:
    LutInversePass(VulkanContext* ctx, const Lut3D& lut);
    ~LutInversePass();

    LutInversePass(const LutInversePass&) = delete;
    LutInversePass& operator=(const LutInversePass&) = delete;

    void execute(VkCommandBuffer cmd,
                 PooledTextureHandle srcDisplay,
                 PooledTextureHandle dstSceneLinear);

private:
    void createDescriptorSetLayout();
    void createPipeline();
    void createSampler();
    void uploadLut(const Lut3D& lut);
    void destroyLut();

    VulkanContext* m_ctx = nullptr;
    VkShaderModule m_shaderModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkImage m_lutImage = VK_NULL_HANDLE;
    VkImageView m_lutView = VK_NULL_HANDLE;
    VmaAllocation m_lutAllocation = VK_NULL_HANDLE;
    float m_domainMin[3] = {0.0f, 0.0f, 0.0f};
    float m_domainScale[3] = {1.0f, 1.0f, 1.0f};
};

} // namespace dlss
