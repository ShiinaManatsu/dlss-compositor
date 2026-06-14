// =============================================================================
// InverseTonemapPass — Stage 6: display-ready → scene-linear
//
// Called ONLY in FG pipeline to convert FG output back to scene-linear for
// EXR writing. SR-only path skips this pass entirely.
//
// LDR mode: inverse sRGB EOTF (IEC 61966-2-1) only. No inverse tonemap.
// HDR mode: inverse PQ ST.2084 (SMPTE / ITU-R BT.2100) + inverse ACES Filmic
//           (Narkowicz approximation quadratic inversion).
//           Blender convention: 1.0 scene-linear = 100 nits diffuse white.
//
// Symmetric inverse of Stage 4 (tonemap_pass.cpp / TonemapPass).
// =============================================================================

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "inverse_tonemap_pass.h"
#include <volk.h>
#include <stdexcept>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dlss {

namespace {

std::vector<uint32_t> readSpirv(const char* fileName) {
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
            throw std::runtime_error("InverseTonemapPass: invalid SPIR-V size: " + path.string());
        }
        std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("InverseTonemapPass: failed to read SPIR-V: " + path.string());
        }
        return code;
    }
    throw std::runtime_error(std::string("InverseTonemapPass: SPIR-V not found: ") + fileName);
}

} // namespace

// ---------------------------------------------------------------------------
// Push-constant layout must exactly match the GLSL struct in inverse_tonemap_eotf.comp
// ---------------------------------------------------------------------------
struct InverseTonemapPushConstants {
    int32_t mode;  // 0 = LDR, 1 = HDR
};
static_assert(sizeof(InverseTonemapPushConstants) == 4,
              "InverseTonemapPushConstants size mismatch with shader push_constant block");

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

InverseTonemapPass::InverseTonemapPass(VulkanContext* ctx)
    : m_ctx(ctx)
{
    createDescriptorSetLayout();
    createPipeline();
}

InverseTonemapPass::~InverseTonemapPass()
{
    VkDevice dev = m_ctx->device();
    if (m_pipeline       != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_sampler        != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_descPool       != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descSetLayout  != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_descSetLayout, nullptr);
    if (m_shaderModule   != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_shaderModule, nullptr);
}

// ---------------------------------------------------------------------------
// createDescriptorSetLayout
//   binding 0: combined image sampler (srcDisplay)
//   binding 1: storage image          (dstSceneLinear)
// ---------------------------------------------------------------------------
void InverseTonemapPass::createDescriptorSetLayout()
{
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    // binding 0 — sampler2D srcDisplay
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = nullptr;

    // binding 1 — image2D dstLinear (storage)
    bindings[1].binding            = 1;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    if (vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create descriptor set layout");
    }

    // Descriptor pool: one set per execute() call.
    // Pre-allocate a modest pool; sets freed via FREE_DESCRIPTOR_SET_BIT.
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 16;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();

    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descSetLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to allocate descriptor set");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create sampler");
    }
}

// ---------------------------------------------------------------------------
// createPipeline
//   Loads the SPIR-V blob (embedded or from disk) and builds the compute pipeline.
// ---------------------------------------------------------------------------
void InverseTonemapPass::createPipeline()
{
    VkDevice dev = m_ctx->device();

    // ---- Load SPIR-V -------------------------------------------------------
    const std::vector<uint32_t> code = readSpirv("inverse_tonemap_eotf.comp.spv");
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size() * sizeof(uint32_t);
    moduleInfo.pCode = code.data();

    if (vkCreateShaderModule(dev, &moduleInfo, nullptr, &m_shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create shader module");
    }

    // ---- Pipeline layout (push constants + descriptor set) -----------------
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(InverseTonemapPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_descSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create pipeline layout");
    }

    // ---- Compute pipeline --------------------------------------------------
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = m_shaderModule;
    stageInfo.pName  = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("InverseTonemapPass: failed to create compute pipeline");
    }
}

// ---------------------------------------------------------------------------
// execute — record dispatch into the provided command buffer
// ---------------------------------------------------------------------------
void InverseTonemapPass::execute(VkCommandBuffer     cmd,
                                  PooledTextureHandle srcDisplay,
                                  PooledTextureHandle dstSceneLinear,
                                  TonemapMode         mode)
{
    VkDevice dev = m_ctx->device();

    // ---- Update descriptor set ---------------------------------------------
    VkDescriptorImageInfo srcImageInfo{};
    srcImageInfo.sampler     = m_sampler;
    srcImageInfo.imageView   = srcDisplay->view;
    srcImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dstImageInfo{};
    dstImageInfo.sampler     = VK_NULL_HANDLE;
    dstImageInfo.imageView   = dstSceneLinear->view;
    dstImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo      = &srcImageInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo      = &dstImageInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // ---- Record dispatch ---------------------------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    InverseTonemapPushConstants pc{};
    pc.mode = static_cast<int32_t>(mode);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);

    // Dispatch: 8×8 local group size (matches shader layout)
    const uint32_t w  = dstSceneLinear->desc.width;
    const uint32_t h  = dstSceneLinear->desc.height;
    const uint32_t gx = (w + 7u) / 8u;
    const uint32_t gy = (h + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);

}

} // namespace dlss
