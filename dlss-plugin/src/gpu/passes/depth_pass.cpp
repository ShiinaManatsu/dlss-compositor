// =============================================================================
// DepthConversionPass — world-linear → NDC [0,1] depth conversion.
//
// DLSS SR §3.5 requires depth in NDC [0,1] range, non-inverted.
// Blender Cycles Z pass provides positive world-space distances (NOT NDC).
// This pass applies the camera projection matrix to produce correct NDC depth.
//
// Image layout requirements (caller responsibility):
//   srcWorldLinear : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
//   dstNdc         : VK_IMAGE_LAYOUT_GENERAL
// =============================================================================

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "depth_pass.h"
#include <spdlog/spdlog.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Resolve volk function pointers (VOLK_IMPLEMENTATION defined once in vulkan_context.cpp)
#include <volk.h>

namespace dlss {
// Push constant layout (must match depth_world_to_ndc.comp)
// ---------------------------------------------------------------------------
struct DepthPushConstants {
    float projection[4][4];  // column-major 4x4 projection matrix
    float nearClip;
    float farClip;
};

// ---------------------------------------------------------------------------
// SPIR-V loading helper
// ---------------------------------------------------------------------------
std::vector<uint32_t> loadSpirv(const char* fileName) {
    const std::array<std::filesystem::path, 3> candidates{
        std::filesystem::path(DLSS_SHADER_DIR) / fileName,
        std::filesystem::current_path() / "shaders" / fileName,
        std::filesystem::current_path() / fileName,
    };

    for (const auto& path : candidates) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) continue;

        const auto size = in.tellg();
        if (size <= 0 || (size % 4) != 0) {
            throw std::runtime_error("DepthConversionPass: invalid SPIR-V size for: " + path.string());
        }

        std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("DepthConversionPass: read error for: " + path.string());
        }
        return code;
    }

    throw std::runtime_error(std::string("DepthConversionPass: SPIR-V not found: ") + fileName);
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
DepthConversionPass::DepthConversionPass(VulkanContext* ctx)
    : m_ctx(ctx)
{
    createDescriptorSetLayout();
    createPipeline();
    spdlog::info("DepthConversionPass: initialized (DLSS SR §3.5 world-linear→NDC)");
}

DepthConversionPass::~DepthConversionPass() {
    VkDevice dev = m_ctx->device();

    if (m_pipeline        != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout  != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descPool        != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descSetLayout   != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_descSetLayout, nullptr);
    if (m_shaderModule    != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_shaderModule, nullptr);
    if (m_sampler         != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
}

// ---------------------------------------------------------------------------
// createDescriptorSetLayout
//   binding 0: combined image sampler (srcWorldLinear)
//   binding 1: storage image          (dstNdc)
// ---------------------------------------------------------------------------
void DepthConversionPass::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding bindings[2]{};

    // Binding 0: source depth sampler
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: destination NDC storage image
    bindings[1].binding            = 1;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 2;
    ci.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(m_ctx->device(), &ci, nullptr, &m_descSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create descriptor set layout");
    }

    // Create one reusable descriptor set. The pipeline submits and waits once per frame.
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(m_ctx->device(), &poolCI, nullptr, &m_descPool) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descSetLayout;

    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to allocate descriptor set");
    }

    VkSamplerCreateInfo samplerCI{};
    samplerCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter    = VK_FILTER_NEAREST;
    samplerCI.minFilter    = VK_FILTER_NEAREST;
    samplerCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_ctx->device(), &samplerCI, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create sampler");
    }
}

// ---------------------------------------------------------------------------
// createPipeline
//   Loads SPIR-V from depth_world_to_ndc.comp.spv (next to executable).
// ---------------------------------------------------------------------------
void DepthConversionPass::createPipeline() {
    // Locate shader relative to executable
    std::vector<uint32_t> code;
    code = loadSpirv("depth_world_to_ndc.comp.spv");

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = code.size() * sizeof(uint32_t);
    smCI.pCode    = code.data();

    if (vkCreateShaderModule(m_ctx->device(), &smCI, nullptr, &m_shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create shader module");
    }

    // Push constants: DepthPushConstants (mat4 + 2 floats = 72 bytes)
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(DepthPushConstants);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;

    if (vkCreatePipelineLayout(m_ctx->device(), &layoutCI, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create pipeline layout");
    }

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = m_shaderModule;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_pipelineLayout;

    if (vkCreateComputePipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipeCI, nullptr,
                                 &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("DepthConversionPass: failed to create compute pipeline");
    }
}

// ---------------------------------------------------------------------------
// execute
//   Records depth conversion dispatch into `cmd`.
//   Image layout pre-conditions (caller must transition before calling):
//     srcWorldLinear: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
//     dstNdc:         VK_IMAGE_LAYOUT_GENERAL
// ---------------------------------------------------------------------------
void DepthConversionPass::execute(VkCommandBuffer     cmd,
                                   PooledTextureHandle srcWorldLinear,
                                   PooledTextureHandle dstNdc,
                                   const float         projection[4][4],
                                   float               nearClip,
                                   float               farClip)
{
    VkDevice dev = m_ctx->device();

    if (m_descSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) {
        throw std::runtime_error("DepthConversionPass: descriptor resources are not initialized");
    }

    // Write descriptor bindings
    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler     = m_sampler;
    srcInfo.imageView   = srcWorldLinear->view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.sampler     = VK_NULL_HANDLE;
    dstInfo.imageView   = dstNdc->view;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo      = &srcInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo      = &dstInfo;

    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

    // Build push constants (DLSS SR §3.5 — projection matrix path)
    DepthPushConstants pc{};
    memcpy(pc.projection, projection, sizeof(pc.projection));
    pc.nearClip = nearClip;
    pc.farClip  = farClip;

    // Record commands
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    // Dispatch: local_size 8x8, round up to cover all pixels
    uint32_t width  = dstNdc->desc.width;
    uint32_t height = dstNdc->desc.height;
    uint32_t groupX = (width  + 7u) / 8u;
    uint32_t groupY = (height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);
}

} // namespace dlss
