#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "preview_pass.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace dlss {
namespace {

std::vector<std::uint32_t> readSpirv(const char* fileName) {
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
            throw std::runtime_error("PreviewPass: invalid SPIR-V size: " + path.string());
        }
        std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("PreviewPass: failed to read SPIR-V: " + path.string());
        }
        return code;
    }
    throw std::runtime_error(std::string("PreviewPass: SPIR-V not found: ") + fileName);
}

std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor) {
    return (value + divisor - 1u) / divisor;
}

struct PreviewPushConstants {
    std::int32_t mode = 0;
    std::int32_t hdr = 0;
};
static_assert(sizeof(PreviewPushConstants) == 8);

} // namespace

PreviewPass::PreviewPass(VulkanContext* ctx)
    : m_ctx(ctx), m_pool(ctx, 64ull * 1024ull * 1024ull) {
    if (!m_ctx) {
        throw std::runtime_error("PreviewPass: VulkanContext must not be null");
    }
    createDescriptorSetLayout();
    createPipeline();

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 64;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 64;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    const VkResult result = vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkCreateDescriptorPool failed: " + std::to_string(result));
    }
}

PreviewPass::~PreviewPass() {
    if (!m_ctx) return;
    const VkDevice device = m_ctx->device();
    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(device, m_descPool, nullptr);
    if (m_descSetLayout) vkDestroyDescriptorSetLayout(device, m_descSetLayout, nullptr);
    if (m_shaderModule) vkDestroyShaderModule(device, m_shaderModule, nullptr);
    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
}

void PreviewPass::createDescriptorSetLayout() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    VkResult result = vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkCreateSampler failed: " + std::to_string(result));
    }

    VkDescriptorSetLayoutBinding srcBinding{};
    srcBinding.binding = 0;
    srcBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcBinding.descriptorCount = 1;
    srcBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding dstBinding{};
    dstBinding.binding = 1;
    dstBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dstBinding.descriptorCount = 1;
    dstBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    const std::array bindings{srcBinding, dstBinding};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    result = vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descSetLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkCreateDescriptorSetLayout failed: " + std::to_string(result));
    }
}

void PreviewPass::createPipeline() {
    const auto shaderCode = readSpirv("preview_display.comp.spv");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shaderCode.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = shaderCode.data();
    VkResult result = vkCreateShaderModule(m_ctx->device(), &shaderInfo, nullptr, &m_shaderModule);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkCreateShaderModule failed: " + std::to_string(result));
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PreviewPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    result = vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkCreatePipelineLayout failed: " + std::to_string(result));
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
        throw std::runtime_error("PreviewPass: vkCreateComputePipelines failed: " + std::to_string(result));
    }
}

PooledTextureHandle PreviewPass::acquirePreviewTexture(std::uint32_t srcWidth,
                                                       std::uint32_t srcHeight,
                                                       std::uint32_t maxLongEdge) {
    if (srcWidth == 0 || srcHeight == 0 || maxLongEdge == 0) {
        throw std::runtime_error("PreviewPass: invalid preview dimensions");
    }
    const std::uint32_t longEdge = std::max(srcWidth, srcHeight);
    const double scale = longEdge > maxLongEdge
        ? static_cast<double>(maxLongEdge) / static_cast<double>(longEdge)
        : 1.0;
    const std::uint32_t outW = std::max(1u, static_cast<std::uint32_t>(std::lround(srcWidth * scale)));
    const std::uint32_t outH = std::max(1u, static_cast<std::uint32_t>(std::lround(srcHeight * scale)));
    TextureDesc desc{outW, outH, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    return m_pool.acquire(desc);
}

void PreviewPass::record(VkCommandBuffer cmd,
                         PooledTextureHandle src,
                         PooledTextureHandle dst,
                         PreviewInputMode mode,
                         bool hdr,
                         VkImageLayout srcLayout) {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descSetLayout;
    VkResult result = vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &descriptorSet);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("PreviewPass: vkAllocateDescriptorSets failed: " + std::to_string(result));
    }

    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = m_sampler;
    srcInfo.imageView = src->view;
    srcInfo.imageLayout = srcLayout;

    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView = dst->view;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &dstInfo;
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);

    VkImageMemoryBarrier2 dstToGeneral{};
    dstToGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    dstToGeneral.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    dstToGeneral.srcAccessMask = 0;
    dstToGeneral.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    dstToGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    dstToGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToGeneral.image = dst->image;
    dstToGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &dstToGeneral;
    vkCmdPipelineBarrier2(cmd, &dependency);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    PreviewPushConstants pc{static_cast<std::int32_t>(mode), hdr ? 1 : 0};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, ceilDiv(dst->desc.width, 8), ceilDiv(dst->desc.height, 8), 1);
}

PreviewReadback PreviewPass::renderAndReadback(PooledTextureHandle src,
                                               PreviewInputMode mode,
                                               bool hdr,
                                               VkImageLayout srcLayout,
                                               std::uint32_t maxLongEdge) {
    if (!src) {
        return {};
    }
    auto preview = acquirePreviewTexture(src->desc.width, src->desc.height, maxLongEdge);

    // Halving chain: one bilinear tap per destination pixel aliases badly at
    // ratios above ~2x (fine detail smears — reads as a low-res preview).
    // Step down in ≤2x hops, then make the final ≤2x hop into the preview
    // texture. Intermediate hops run passthrough (DisplayReady + hdr=false) so
    // tonemapping happens exactly once, in the final pass, and keep the source
    // format to avoid extra quantization.
    struct ChainStep {
        PooledTextureHandle tex;
        VkImageLayout       layout;
    };
    std::vector<ChainStep> chain;
    PooledTextureHandle cur = src;
    VkImageLayout curLayout = srcLayout;
    while (cur->desc.width > 2u * preview->desc.width ||
           cur->desc.height > 2u * preview->desc.height) {
        const TextureDesc td{std::max(1u, cur->desc.width / 2),
                             std::max(1u, cur->desc.height / 2),
                             src->desc.format,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        chain.push_back({m_pool.acquire(td), VK_IMAGE_LAYOUT_GENERAL});
        cur = chain.back().tex;
        curLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkCommandPool cmdPool = m_ctx->createComputeCommandPool();
    VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
    PooledTextureHandle stepSrc = src;
    VkImageLayout stepLayout = srcLayout;
    for (const auto& step : chain) {
        record(cmd, stepSrc, step.tex, PreviewInputMode::DisplayReady, /*hdr=*/false, stepLayout);
        stepSrc = step.tex;
        stepLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    record(cmd, stepSrc, preview, mode, hdr, stepLayout);

    VkImageMemoryBarrier2 toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toTransfer.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = preview->image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(cmd, &dependency);

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(preview->desc.width) * preview->desc.height * 4u;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    VkResult result = vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo);
    if (result != VK_SUCCESS) {
        m_ctx->destroyCommandPool(cmdPool);
        m_pool.release(preview);
        throw std::runtime_error("PreviewPass: failed to create readback buffer");
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {preview->desc.width, preview->desc.height, 1};
    vkCmdCopyImageToBuffer(cmd, preview->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());
    m_ctx->destroyCommandPool(cmdPool);
    vkResetDescriptorPool(m_ctx->device(), m_descPool, 0);

    PreviewReadback readback{preview->desc.width, preview->desc.height, std::vector<std::uint8_t>(static_cast<std::size_t>(byteSize))};
    if (allocationInfo.pMappedData) {
        std::memcpy(readback.rgba8.data(), allocationInfo.pMappedData, readback.rgba8.size());
    }
    vmaDestroyBuffer(m_ctx->allocator(), buffer, allocation);
    m_pool.release(preview);
    for (const auto& step : chain) {
        m_pool.release(step.tex);
    }
    return readback;
}

} // namespace dlss
