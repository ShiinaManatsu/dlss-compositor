#include "mv_pass.h"

#include <volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef DLSS_SHADER_DIR
#define DLSS_SHADER_DIR "."
#endif

namespace dlss {

namespace {

std::vector<uint32_t> readSpirv(const char* fileName) {
    const std::array<std::filesystem::path, 4> candidates{
        std::filesystem::path(DLSS_SHADER_DIR) / fileName,
        std::filesystem::path(DLSS_SHADER_DIR) / "mv_blender_to_dlss.comp.spv",
        std::filesystem::current_path() / "shaders" / fileName,
        std::filesystem::current_path() / fileName,
    };

    for (const auto& path : candidates) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) continue;

        const auto size = in.tellg();
        if (size <= 0 || (size % 4) != 0) {
            throw std::runtime_error("Invalid SPIR-V size: " + path.string());
        }

        std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("Failed to read SPIR-V: " + path.string());
        }
        return code;
    }

    throw std::runtime_error(std::string("SPIR-V not found: ") + fileName);
}

uint32_t ceilDiv(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

} // namespace

MvConversionPass::MvConversionPass(VulkanContext* ctx)
    : m_ctx(ctx) {
    if (!m_ctx) {
        throw std::runtime_error("MvConversionPass: VulkanContext must not be null");
    }

    createDescriptorSetLayout();
    createPipeline();

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1024;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1024;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1024;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes    = poolSizes;

    const VkResult result = vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreateDescriptorPool failed: " + std::to_string(result));
    }
}

MvConversionPass::~MvConversionPass() {
    if (!m_ctx) return;

    const VkDevice device = m_ctx->device();
    if (m_pipeline)       vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_descPool)       vkDestroyDescriptorPool(device, m_descPool, nullptr);
    if (m_descSetLayout)  vkDestroyDescriptorSetLayout(device, m_descSetLayout, nullptr);
    if (m_shaderModule)   vkDestroyShaderModule(device, m_shaderModule, nullptr);
    if (m_sampler)        vkDestroySampler(device, m_sampler, nullptr);
}

void MvConversionPass::createDescriptorSetLayout() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_NEAREST;
    samplerInfo.minFilter    = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = 1.0f;

    VkResult result = vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreateSampler failed: " + std::to_string(result));
    }

    VkDescriptorSetLayoutBinding srcBinding{};
    srcBinding.binding         = 0;
    srcBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcBinding.descriptorCount = 1;
    srcBinding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding dstBinding{};
    dstBinding.binding         = 1;
    dstBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dstBinding.descriptorCount = 1;
    dstBinding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    const std::array bindings{srcBinding, dstBinding};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    result = vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descSetLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreateDescriptorSetLayout failed: " + std::to_string(result));
    }
}

void MvConversionPass::createPipeline() {
    const std::vector<uint32_t> shaderCode = readSpirv("mv_blender_to_dlss.comp.spv");

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shaderCode.size() * sizeof(uint32_t);
    shaderInfo.pCode    = shaderCode.data();

    VkResult result = vkCreateShaderModule(m_ctx->device(), &shaderInfo, nullptr, &m_shaderModule);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreateShaderModule failed: " + std::to_string(result));
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &m_descSetLayout;

    result = vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreatePipelineLayout failed: " + std::to_string(result));
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = m_shaderModule;
    stageInfo.pName  = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;

    result = vkCreateComputePipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass: vkCreateComputePipelines failed: " + std::to_string(result));
    }
}

void MvConversionPass::execute(VkCommandBuffer cmd,
                               PooledTextureHandle srcBlender4ch,
                               PooledTextureHandle dstDlss2ch) {
    if (cmd == VK_NULL_HANDLE) {
        throw std::runtime_error("MvConversionPass::execute: null command buffer");
    }
    if (!srcBlender4ch || !dstDlss2ch) {
        throw std::runtime_error("MvConversionPass::execute: null texture handle");
    }
    if (srcBlender4ch->view == VK_NULL_HANDLE || dstDlss2ch->view == VK_NULL_HANDLE) {
        throw std::runtime_error("MvConversionPass::execute: texture image view is null");
    }
    if (srcBlender4ch->desc.width != dstDlss2ch->desc.width ||
        srcBlender4ch->desc.height != dstDlss2ch->desc.height) {
        throw std::runtime_error("MvConversionPass::execute: source and destination dimensions differ");
    }

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descSetLayout;

    VkResult result = vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("MvConversionPass::execute: vkAllocateDescriptorSets failed: " + std::to_string(result));
    }

    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler     = m_sampler;
    srcInfo.imageView   = srcBlender4ch->view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView   = dstDlss2ch->view;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo      = &srcInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo      = &dstInfo;

    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);

    VkImageMemoryBarrier2 dstToGeneral{};
    dstToGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    dstToGeneral.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
    dstToGeneral.srcAccessMask       = 0;
    dstToGeneral.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    dstToGeneral.dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    dstToGeneral.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    dstToGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    dstToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.image               = dstDlss2ch->image;
    dstToGeneral.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    dstToGeneral.subresourceRange.baseMipLevel   = 0;
    dstToGeneral.subresourceRange.levelCount     = 1;
    dstToGeneral.subresourceRange.baseArrayLayer = 0;
    dstToGeneral.subresourceRange.layerCount     = 1;

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &dstToGeneral;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);

    vkCmdDispatch(cmd, ceilDiv(dstDlss2ch->desc.width, 8), ceilDiv(dstDlss2ch->desc.height, 8), 1);
}

} // namespace dlss
