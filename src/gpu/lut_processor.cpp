#include "gpu/lut_processor.h"

#include "gpu/vulkan_context.h"

#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

void checkVkResult(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message != nullptr ? message : "Vulkan call failed.");
    }
}

void checkVmaResult(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message != nullptr ? message : "VMA call failed.");
    }
}

VkCommandBuffer beginOneTimeCommands(VulkanContext& ctx) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = ctx.commandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    checkVkResult(vkAllocateCommandBuffers(ctx.device(), &allocInfo, &cmdBuf),
                  "Failed to allocate one-time command buffer.");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVkResult(vkBeginCommandBuffer(cmdBuf, &beginInfo), "Failed to begin one-time command buffer.");
    return cmdBuf;
}

void endOneTimeCommands(VulkanContext& ctx, VkCommandBuffer cmdBuf) {
    checkVkResult(vkEndCommandBuffer(cmdBuf), "Failed to end one-time command buffer.");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    checkVkResult(vkQueueSubmit(ctx.computeQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit one-time command buffer.");
    checkVkResult(vkQueueWaitIdle(ctx.computeQueue()), "Failed waiting for compute queue idle.");
    vkFreeCommandBuffers(ctx.device(), ctx.commandPool(), 1, &cmdBuf);
}

} // namespace

LutProcessor::LutProcessor(VulkanContext& ctx) : m_ctx(ctx) {}

LutProcessor::~LutProcessor() {
    destroy();
}

bool LutProcessor::loadLut(const std::string& filepath, int lutSize, std::string& errorMsg) {
    if (m_lutLoaded) {
        destroyLut();
    }

    // Read binary file
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        errorMsg = "Failed to open LUT file: " + filepath;
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    const std::streamsize expectedSize = static_cast<std::streamsize>(lutSize) *
                                         static_cast<std::streamsize>(lutSize) *
                                         static_cast<std::streamsize>(lutSize) *
                                         4 * sizeof(float);
    if (fileSize != expectedSize) {
        errorMsg = "LUT file size mismatch. Expected " + std::to_string(expectedSize) +
                   " bytes, got " + std::to_string(fileSize) + " bytes.";
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<float> lutData(static_cast<size_t>(fileSize / sizeof(float)));
    file.read(reinterpret_cast<char*>(lutData.data()), fileSize);
    if (!file.good()) {
        errorMsg = "Failed to read LUT file: " + filepath;
        return false;
    }
    file.close();

    m_lutSize = lutSize;

    // Create staging buffer
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(fileSize);

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;

    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = imageSize;
    bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    try {
        checkVmaResult(vmaCreateBuffer(m_ctx.allocator(),
                                       &bufferCreateInfo,
                                       &stagingAllocInfo,
                                       &stagingBuffer,
                                       &stagingAllocation,
                                       nullptr),
                       "Failed to create LUT staging buffer.");

        void* mappedData = nullptr;
        checkVmaResult(vmaMapMemory(m_ctx.allocator(), stagingAllocation, &mappedData),
                       "Failed to map LUT staging buffer.");
        std::memcpy(mappedData, lutData.data(), static_cast<size_t>(imageSize));
        checkVmaResult(vmaFlushAllocation(m_ctx.allocator(), stagingAllocation, 0, imageSize),
                       "Failed to flush LUT staging buffer.");
        vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);

        // Create 3D image
        VkImageCreateInfo imageCreateInfo{};
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.imageType = VK_IMAGE_TYPE_3D;
        imageCreateInfo.extent = {static_cast<uint32_t>(lutSize),
                                  static_cast<uint32_t>(lutSize),
                                  static_cast<uint32_t>(lutSize)};
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo imageAllocInfo{};
        imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        checkVmaResult(vmaCreateImage(m_ctx.allocator(),
                                      &imageCreateInfo,
                                      &imageAllocInfo,
                                      &m_lutImage,
                                      &m_lutAllocation,
                                      nullptr),
                       "Failed to create 3D LUT image.");

        // Create image view
        VkImageViewCreateInfo viewCreateInfo{};
        viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = m_lutImage;
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewCreateInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreateInfo.subresourceRange.baseMipLevel = 0;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.baseArrayLayer = 0;
        viewCreateInfo.subresourceRange.layerCount = 1;

        checkVkResult(vkCreateImageView(m_ctx.device(), &viewCreateInfo, nullptr, &m_lutView),
                      "Failed to create 3D LUT image view.");

        // Create sampler (trilinear, clamp to edge)
        VkSamplerCreateInfo samplerCreateInfo{};
        samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
        samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
        samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCreateInfo.minLod = 0.0f;
        samplerCreateInfo.maxLod = 0.0f;

        checkVkResult(vkCreateSampler(m_ctx.device(), &samplerCreateInfo, nullptr, &m_lutSampler),
                      "Failed to create LUT sampler.");

        // Upload data: transition, copy, transition
        VkCommandBuffer cmdBuf = beginOneTimeCommands(m_ctx);

        // Transition to transfer dst
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_lutImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmdBuf,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy buffer to 3D image
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = {static_cast<uint32_t>(lutSize),
                                  static_cast<uint32_t>(lutSize),
                                  static_cast<uint32_t>(lutSize)};

        vkCmdCopyBufferToImage(cmdBuf,
                               stagingBuffer,
                               m_lutImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copyRegion);

        // Transition to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmdBuf,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        endOneTimeCommands(m_ctx, cmdBuf);

        // Cleanup staging
        vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);

        m_lutLoaded = true;
        std::fprintf(stdout, "Loaded 3D LUT: %s (%d^3, %.1f MB)\n",
                     filepath.c_str(), lutSize,
                     static_cast<float>(fileSize) / (1024.0f * 1024.0f));
        return true;

    } catch (const std::exception& ex) {
        if (stagingBuffer != VK_NULL_HANDLE || stagingAllocation != nullptr) {
            vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        }
        destroyLut();
        errorMsg = ex.what();
        return false;
    }
}

bool LutProcessor::createPipeline(const std::string& spvPath, std::string& errorMsg) {
    if (m_pipelineCreated) {
        destroyPipeline();
    }

    // Read SPIR-V binary
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        errorMsg = "Failed to open SPIR-V file: " + spvPath;
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> spvCode(static_cast<size_t>(fileSize));
    file.read(spvCode.data(), fileSize);
    if (!file.good()) {
        errorMsg = "Failed to read SPIR-V file: " + spvPath;
        return false;
    }
    file.close();

    try {
        // Create shader module
        VkShaderModuleCreateInfo shaderCreateInfo{};
        shaderCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderCreateInfo.codeSize = static_cast<size_t>(fileSize);
        shaderCreateInfo.pCode = reinterpret_cast<const uint32_t*>(spvCode.data());

        checkVkResult(vkCreateShaderModule(m_ctx.device(), &shaderCreateInfo, nullptr, &m_shaderModule),
                      "Failed to create LUT shader module.");

        // Descriptor set layout:
        //   binding 0: input image (storage image, read-only)
        //   binding 1: output image (storage image, write-only)
        //   binding 2: LUT sampler (combined image sampler)
        VkDescriptorSetLayoutBinding bindings[3] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.bindingCount = 3;
        layoutCreateInfo.pBindings = bindings;

        checkVkResult(vkCreateDescriptorSetLayout(m_ctx.device(), &layoutCreateInfo, nullptr,
                                                   &m_descriptorSetLayout),
                      "Failed to create LUT descriptor set layout.");

        // Push constant range
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(float) * 2 + sizeof(int) * 3; // shaperMin, shaperMax, width, height, useLogShaper

        // Pipeline layout
        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &m_descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
        pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

        checkVkResult(vkCreatePipelineLayout(m_ctx.device(), &pipelineLayoutCreateInfo, nullptr,
                                              &m_pipelineLayout),
                      "Failed to create LUT pipeline layout.");

        // Compute pipeline
        VkComputePipelineCreateInfo pipelineCreateInfo{};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineCreateInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineCreateInfo.stage.module = m_shaderModule;
        pipelineCreateInfo.stage.pName = "main";
        pipelineCreateInfo.layout = m_pipelineLayout;

        checkVkResult(vkCreateComputePipelines(m_ctx.device(), VK_NULL_HANDLE, 1,
                                                &pipelineCreateInfo, nullptr, &m_pipeline),
                      "Failed to create LUT compute pipeline.");

        // Descriptor pool (enough for many dispatches - we allocate per-apply)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 64; // 32 dispatches * 2 images
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 32;

        VkDescriptorPoolCreateInfo poolCreateInfo{};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolCreateInfo.maxSets = 32;
        poolCreateInfo.poolSizeCount = 2;
        poolCreateInfo.pPoolSizes = poolSizes;

        checkVkResult(vkCreateDescriptorPool(m_ctx.device(), &poolCreateInfo, nullptr, &m_descriptorPool),
                      "Failed to create LUT descriptor pool.");

        m_pipelineCreated = true;
        return true;

    } catch (const std::exception& ex) {
        destroyPipeline();
        errorMsg = ex.what();
        return false;
    }
}

void LutProcessor::apply(VkCommandBuffer cmdBuf,
                          VkImage inputImage,
                          VkImageView inputView,
                          VkImage outputImage,
                          VkImageView outputView,
                          int width,
                          int height,
                          bool useLogShaper) {
    if (!m_lutLoaded || !m_pipelineCreated) {
        throw std::runtime_error("LutProcessor::apply called before LUT/pipeline are ready.");
    }

    // Allocate descriptor set
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    checkVkResult(vkAllocateDescriptorSets(m_ctx.device(), &allocInfo, &descriptorSet),
                  "Failed to allocate LUT descriptor set.");

    // Update descriptor set
    VkDescriptorImageInfo inputImageInfo{};
    inputImageInfo.imageView = inputView;
    inputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo outputImageInfo{};
    outputImageInfo.imageView = outputView;
    outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo lutImageInfo{};
    lutImageInfo.sampler = m_lutSampler;
    lutImageInfo.imageView = m_lutView;
    lutImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &inputImageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputImageInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &lutImageInfo;

    vkUpdateDescriptorSets(m_ctx.device(), 3, writes, 0, nullptr);

    // Bind pipeline and descriptor set
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmdBuf,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout,
                            0, 1, &descriptorSet,
                            0, nullptr);

    // Push constants
    struct PushConstants {
        float shaperLog2Min;
        float shaperLog2Max;
        int imageWidth;
        int imageHeight;
        int useLogShaper;
    } pc{};
    pc.shaperLog2Min = kShaperLog2Min;
    pc.shaperLog2Max = kShaperLog2Max;
    pc.imageWidth = width;
    pc.imageHeight = height;
    pc.useLogShaper = useLogShaper ? 1 : 0;

    vkCmdPushConstants(cmdBuf,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(PushConstants),
                       &pc);

    // Dispatch compute shader
    const uint32_t groupCountX = (static_cast<uint32_t>(width) + 15u) / 16u;
    const uint32_t groupCountY = (static_cast<uint32_t>(height) + 15u) / 16u;
    vkCmdDispatch(cmdBuf, groupCountX, groupCountY, 1);

    // Memory barrier after compute write, before next operation
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmdBuf,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,
                         1, &memoryBarrier,
                         0, nullptr,
                         0, nullptr);

    // Free descriptor set (pool has FREE_DESCRIPTOR_SET_BIT)
    vkFreeDescriptorSets(m_ctx.device(), m_descriptorPool, 1, &descriptorSet);
}

void LutProcessor::destroy() {
    destroyPipeline();
    destroyLut();
}

bool LutProcessor::isReady() const {
    return m_lutLoaded && m_pipelineCreated;
}

void LutProcessor::destroyLut() {
    if (m_lutSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx.device(), m_lutSampler, nullptr);
        m_lutSampler = VK_NULL_HANDLE;
    }
    if (m_lutView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx.device(), m_lutView, nullptr);
        m_lutView = VK_NULL_HANDLE;
    }
    if (m_lutImage != VK_NULL_HANDLE || m_lutAllocation != nullptr) {
        vmaDestroyImage(m_ctx.allocator(), m_lutImage, m_lutAllocation);
        m_lutImage = VK_NULL_HANDLE;
        m_lutAllocation = nullptr;
    }
    m_lutSize = 0;
    m_lutLoaded = false;
}

void LutProcessor::destroyPipeline() {
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx.device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx.device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx.device(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx.device(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_ctx.device(), m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
    }
    m_pipelineCreated = false;
}
