#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "lut_inverse_pass.h"

#include <volk.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

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
            throw std::runtime_error("LutInversePass: invalid SPIR-V size: " + path.string());
        }
        std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
        if (!in) {
            throw std::runtime_error("LutInversePass: failed to read SPIR-V: " + path.string());
        }
        return code;
    }
    throw std::runtime_error(std::string("LutInversePass: SPIR-V not found: ") + fileName);
}

std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor) {
    return (value + divisor - 1u) / divisor;
}

struct LutPushConstants {
    float domainMin[4];
    float domainScale[4];
};
static_assert(sizeof(LutPushConstants) == 32);

} // namespace

LutInversePass::LutInversePass(VulkanContext* ctx, const Lut3D& lut)
    : m_ctx(ctx) {
    if (!m_ctx) {
        throw std::runtime_error("LutInversePass: VulkanContext must not be null");
    }
    if (lut.size <= 1) {
        throw std::runtime_error("LutInversePass: LUT size must be greater than 1");
    }
    createSampler();
    createDescriptorSetLayout();
    createPipeline();
    uploadLut(lut);
}

LutInversePass::~LutInversePass() {
    if (!m_ctx) return;
    const VkDevice device = m_ctx->device();
    if (m_pipeline) vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(device, m_descPool, nullptr);
    if (m_descSetLayout) vkDestroyDescriptorSetLayout(device, m_descSetLayout, nullptr);
    if (m_shaderModule) vkDestroyShaderModule(device, m_shaderModule, nullptr);
    if (m_sampler) vkDestroySampler(device, m_sampler, nullptr);
    destroyLut();
}

void LutInversePass::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create sampler");
    }
}

void LutInversePass::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_descSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create descriptor set layout");
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_descPool) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descSetLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to allocate descriptor set");
    }
}

void LutInversePass::createPipeline() {
    const auto code = readSpirv("lut_inverse.comp.spv");
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = code.data();
    if (vkCreateShaderModule(m_ctx->device(), &moduleInfo, nullptr, &m_shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create shader module");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(LutPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create pipeline layout");
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
    if (vkCreateComputePipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create compute pipeline");
    }
}

void LutInversePass::uploadLut(const Lut3D& lut) {
    for (int i = 0; i < 3; ++i) {
        m_domainMin[i] = lut.domain_min[i];
        const float width = lut.domain_max[i] - lut.domain_min[i];
        m_domainScale[i] = width != 0.0f ? 1.0f / width : 1.0f;
    }
    std::vector<float> rgba(static_cast<std::size_t>(lut.size) * lut.size * lut.size * 4U);
    for (std::size_t src = 0, dst = 0; src < lut.rgb.size(); src += 3U, dst += 4U) {
        rgba[dst + 0U] = lut.rgb[src + 0U];
        rgba[dst + 1U] = lut.rgb[src + 1U];
        rgba[dst + 2U] = lut.rgb[src + 2U];
        rgba[dst + 3U] = 1.0f;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageInfo.extent = {static_cast<std::uint32_t>(lut.size), static_cast<std::uint32_t>(lut.size), static_cast<std::uint32_t>(lut.size)};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkResult result = vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_lutImage, &m_lutAllocation, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("LutInversePass: failed to create LUT image");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_lutImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_lutView);
    if (result != VK_SUCCESS) {
        destroyLut();
        throw std::runtime_error("LutInversePass: failed to create LUT image view");
    }

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(rgba.size() * sizeof(float));
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    result = vmaCreateBuffer(m_ctx->allocator(), &bufferInfo, &stagingAllocInfo, &stagingBuffer, &stagingAllocation, &stagingInfo);
    if (result != VK_SUCCESS || stagingInfo.pMappedData == nullptr) {
        if (stagingBuffer && stagingAllocation) {
            vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAllocation);
        }
        destroyLut();
        throw std::runtime_error("LutInversePass: failed to create LUT staging buffer");
    }
    std::memcpy(stagingInfo.pMappedData, rgba.data(), static_cast<std::size_t>(byteSize));

    VkCommandPool commandPool = m_ctx->createTransferCommandPool();
    VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(commandPool);
    VkImageMemoryBarrier2 toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = m_lutImage;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<std::uint32_t>(lut.size), static_cast<std::uint32_t>(lut.size), static_cast<std::uint32_t>(lut.size)};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_lutImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = m_lutImage;
    toShaderRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dependency.pImageMemoryBarriers = &toShaderRead;
    vkCmdPipelineBarrier2(cmd, &dependency);
    m_ctx->endOneTimeCommands(commandPool, cmd, m_ctx->transferQueue());
    vkDestroyCommandPool(m_ctx->device(), commandPool, nullptr);
    vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAllocation);
}

void LutInversePass::destroyLut() {
    if (m_lutView) {
        vkDestroyImageView(m_ctx->device(), m_lutView, nullptr);
        m_lutView = VK_NULL_HANDLE;
    }
    if (m_lutImage && m_lutAllocation) {
        vmaDestroyImage(m_ctx->allocator(), m_lutImage, m_lutAllocation);
        m_lutImage = VK_NULL_HANDLE;
        m_lutAllocation = VK_NULL_HANDLE;
    }
}

void LutInversePass::execute(VkCommandBuffer cmd,
                             PooledTextureHandle srcDisplay,
                             PooledTextureHandle dstSceneLinear) {
    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = m_sampler;
    srcInfo.imageView = srcDisplay->view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo lutInfo{};
    lutInfo.sampler = m_sampler;
    lutInfo.imageView = m_lutView;
    lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo dstInfo{};
    dstInfo.imageView = dstSceneLinear->view;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (auto& write : writes) {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descSet;
        write.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &lutInfo;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &dstInfo;
    vkUpdateDescriptorSets(m_ctx->device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
    LutPushConstants pc{};
    for (int i = 0; i < 3; ++i) {
        pc.domainMin[i] = m_domainMin[i];
        pc.domainScale[i] = m_domainScale[i];
    }
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, ceilDiv(dstSceneLinear->desc.width, 8), ceilDiv(dstSceneLinear->desc.height, 8), 1);
}

} // namespace dlss
