#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "prores_convert_pass.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace dlss {
namespace {

std::vector<std::uint32_t> readSpirv(const char* fileName)
{
    const std::array<std::filesystem::path, 3> candidates{
        std::filesystem::path(DLSS_SHADER_DIR) / fileName,
        std::filesystem::current_path() / "shaders" / fileName,
        std::filesystem::current_path() / fileName,
    };
    for (const auto& path : candidates) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            continue;
        }
        const auto size = in.tellg();
        if (size <= 0 || (size % 4) != 0) {
            throw std::runtime_error("ProresConvertPass: invalid SPIR-V size: " + path.string());
        }
        std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("ProresConvertPass: failed to read SPIR-V: " + path.string());
        }
        return code;
    }
    throw std::runtime_error(std::string("ProresConvertPass: SPIR-V not found: ") + fileName);
}

std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor)
{
    return (value + divisor - 1u) / divisor;
}

struct PushConstants {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t hdr = 0;
    std::uint32_t sceneLinear = 0;
};
static_assert(sizeof(PushConstants) == 16);

} // namespace

ProresConvertPass::ProresConvertPass(VulkanContext* ctx) : m_ctx(ctx)
{
    if (!m_ctx) {
        throw std::runtime_error("ProresConvertPass: VulkanContext must not be null");
    }
    createDescriptorSetLayout();
    createPipeline();

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VkResult result = vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreateDescriptorPool failed: " + std::to_string(result));
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descSetLayout;
    result = vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkAllocateDescriptorSets failed: " + std::to_string(result));
    }
}

ProresConvertPass::~ProresConvertPass()
{
    if (!m_ctx) {
        return;
    }
    const VkDevice device = m_ctx->device();
    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(device, m_descPool, nullptr);
    if (m_descSetLayout) vkDestroyDescriptorSetLayout(device, m_descSetLayout, nullptr);
    if (m_shaderModule) vkDestroyShaderModule(device, m_shaderModule, nullptr);
    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
}

void ProresConvertPass::createDescriptorSetLayout()
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    VkResult result = vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreateSampler failed: " + std::to_string(result));
    }

    VkDescriptorSetLayoutBinding srcBinding{};
    srcBinding.binding = 0;
    srcBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcBinding.descriptorCount = 1;
    srcBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding dstBinding{};
    dstBinding.binding = 1;
    dstBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dstBinding.descriptorCount = 1;
    dstBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    const std::array bindings{srcBinding, dstBinding};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    result = vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descSetLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreateDescriptorSetLayout failed: " + std::to_string(result));
    }
}

void ProresConvertPass::createPipeline()
{
    const auto shaderCode = readSpirv("prores_yuv422.comp.spv");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shaderCode.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = shaderCode.data();
    VkResult result = vkCreateShaderModule(m_ctx->device(), &shaderInfo, nullptr, &m_shaderModule);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreateShaderModule failed: " + std::to_string(result));
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    result = vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreatePipelineLayout failed: " + std::to_string(result));
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = m_shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;
    result = vkCreateComputePipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: vkCreateComputePipelines failed: " + std::to_string(result));
    }
}

std::vector<std::uint8_t> ProresConvertPass::convertAndReadback(PooledTextureHandle src,
                                                                PixelEncoding pixelEncoding,
                                                                bool hdr,
                                                                VkImageLayout srcLayout)
{
    if (!src) {
        throw std::runtime_error("ProresConvertPass: source texture is null");
    }
    const std::uint32_t width = src->desc.width;
    const std::uint32_t height = src->desc.height;
    const std::uint32_t chromaWidth = (width + 1u) / 2u;
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(chromaWidth) * height * 2u * sizeof(std::uint32_t);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteSize));

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteSize;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    const VkResult bufferResult = vmaCreateBuffer(
        m_ctx->allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo);
    if (bufferResult != VK_SUCCESS) {
        throw std::runtime_error("ProresConvertPass: failed to create readback buffer");
    }

    VkCommandPool pool = m_ctx->createComputeCommandPool();
    VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(pool);
    record(cmd, src, buffer, width, height, pixelEncoding, hdr, srcLayout);
    m_ctx->endOneTimeCommands(pool, cmd, m_ctx->computeQueue());
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);

    if (allocationInfo.pMappedData != nullptr) {
        std::memcpy(bytes.data(), allocationInfo.pMappedData, bytes.size());
    }
    vmaDestroyBuffer(m_ctx->allocator(), buffer, allocation);
    return bytes;
}

void ProresConvertPass::record(VkCommandBuffer cmd,
                               PooledTextureHandle src,
                               VkBuffer dstBuffer,
                               std::uint32_t width,
                               std::uint32_t height,
                               PixelEncoding pixelEncoding,
                               bool hdr,
                               VkImageLayout srcLayout)
{
    if (m_descSet == VK_NULL_HANDLE) {
        throw std::runtime_error("ProresConvertPass: descriptor set is null");
    }

    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = m_sampler;
    srcInfo.imageView = src->view;
    srcInfo.imageLayout = srcLayout;

    VkDescriptorBufferInfo dstInfo{};
    dstInfo.buffer = dstBuffer;
    dstInfo.offset = 0;
    dstInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &dstInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);

    VkBufferMemoryBarrier2 bufferBarrier{};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    bufferBarrier.srcAccessMask = 0;
    bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = dstBuffer;
    bufferBarrier.offset = 0;
    bufferBarrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &bufferBarrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
    const PushConstants pc{
        width,
        height,
        hdr ? 1u : 0u,
        pixelEncoding == PixelEncoding::SceneLinear ? 1u : 0u,
    };
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, ceilDiv((width + 1u) / 2u, 8u), ceilDiv(height, 8u), 1);

    VkBufferMemoryBarrier2 hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    hostBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = dstBuffer;
    hostBarrier.offset = 0;
    hostBarrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo hostDependency{};
    hostDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    hostDependency.bufferMemoryBarrierCount = 1;
    hostDependency.pBufferMemoryBarriers = &hostBarrier;
    vkCmdPipelineBarrier2(cmd, &hostDependency);
}

} // namespace dlss
