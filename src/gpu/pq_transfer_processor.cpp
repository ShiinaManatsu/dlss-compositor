#include "gpu/pq_transfer_processor.h"

#include "gpu/vulkan_context.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

void checkVkResult(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message != nullptr ? message : "Vulkan call failed.");
    }
}

} // namespace

PqTransferProcessor::PqTransferProcessor(VulkanContext& ctx) : m_ctx(ctx) {}

PqTransferProcessor::~PqTransferProcessor() {
    destroy();
}

bool PqTransferProcessor::createPipeline(const std::string& spvPath, std::string& errorMsg) {
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
                      "Failed to create PQ transfer shader module.");

        // Descriptor set layout: 2 storage images (input + output), no sampler
        VkDescriptorSetLayoutBinding bindings[2] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.bindingCount = 2;
        layoutCreateInfo.pBindings = bindings;

        checkVkResult(vkCreateDescriptorSetLayout(m_ctx.device(), &layoutCreateInfo, nullptr,
                                                   &m_descriptorSetLayout),
                      "Failed to create PQ transfer descriptor set layout.");

        // Push constant range: imageWidth, imageHeight, direction, pad
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(int) * 4;

        // Pipeline layout
        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &m_descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
        pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

        checkVkResult(vkCreatePipelineLayout(m_ctx.device(), &pipelineLayoutCreateInfo, nullptr,
                                              &m_pipelineLayout),
                      "Failed to create PQ transfer pipeline layout.");

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
                      "Failed to create PQ transfer compute pipeline.");

        // Descriptor pool (storage images only, no samplers)
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 64; // 32 dispatches * 2 images

        VkDescriptorPoolCreateInfo poolCreateInfo{};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolCreateInfo.maxSets = 32;
        poolCreateInfo.poolSizeCount = 1;
        poolCreateInfo.pPoolSizes = &poolSize;

        checkVkResult(vkCreateDescriptorPool(m_ctx.device(), &poolCreateInfo, nullptr, &m_descriptorPool),
                      "Failed to create PQ transfer descriptor pool.");

        m_pipelineCreated = true;
        return true;

    } catch (const std::exception& ex) {
        destroyPipeline();
        errorMsg = ex.what();
        return false;
    }
}

void PqTransferProcessor::apply(VkCommandBuffer cmdBuf,
                                 VkImage inputImage,
                                 VkImageView inputView,
                                 VkImage outputImage,
                                 VkImageView outputView,
                                 int width,
                                 int height,
                                 bool encode) {
    if (!m_pipelineCreated) {
        throw std::runtime_error("PqTransferProcessor::apply called before pipeline is ready.");
    }

    // Allocate descriptor set
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    checkVkResult(vkAllocateDescriptorSets(m_ctx.device(), &allocInfo, &descriptorSet),
                  "Failed to allocate PQ transfer descriptor set.");

    // Update descriptor set
    VkDescriptorImageInfo inputImageInfo{};
    inputImageInfo.imageView = inputView;
    inputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo outputImageInfo{};
    outputImageInfo.imageView = outputView;
    outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};

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

    vkUpdateDescriptorSets(m_ctx.device(), 2, writes, 0, nullptr);

    // Bind pipeline and descriptor set
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmdBuf,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout,
                            0, 1, &descriptorSet,
                            0, nullptr);

    // Push constants
    struct PushConstants {
        int imageWidth;
        int imageHeight;
        int direction;
        int _pad;
    } pc{};
    pc.imageWidth = width;
    pc.imageHeight = height;
    pc.direction = encode ? 1 : 0;
    pc._pad = 0;

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

    // Memory barrier after compute write
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

    // Free descriptor set
    vkFreeDescriptorSets(m_ctx.device(), m_descriptorPool, 1, &descriptorSet);
}

void PqTransferProcessor::destroy() {
    destroyPipeline();
}

bool PqTransferProcessor::isReady() const {
    return m_pipelineCreated;
}

void PqTransferProcessor::destroyPipeline() {
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
