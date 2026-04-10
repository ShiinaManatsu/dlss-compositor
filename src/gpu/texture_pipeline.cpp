#include "gpu/texture_pipeline.h"

#include "gpu/vulkan_context.h"

#include <vk_mem_alloc.h>

#include <cstring>
#include <stdexcept>

namespace {

bool isSupportedFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return true;
    default:
        return false;
    }
}

bool isHalfFormat(VkFormat format) {
    return format == VK_FORMAT_R16G16_SFLOAT || format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

uint32_t expectedChannels(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
        return 1;
    case VK_FORMAT_R16G16_SFLOAT:
        return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 4;
    default:
        return 0;
    }
}

uint32_t bytesPerPixel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}

void validateTextureArgs(const float* data, int width, int height, int channels, VkFormat format) {
    if (data == nullptr) {
        throw std::runtime_error("Texture upload received null data pointer.");
    }
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Texture dimensions must be positive.");
    }
    if (!isSupportedFormat(format)) {
        throw std::runtime_error("Texture format is not supported by TexturePipeline.");
    }
    if (channels != static_cast<int>(expectedChannels(format))) {
        throw std::runtime_error("Texture channel count does not match the requested Vulkan format.");
    }
}

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

} // namespace

TexturePipeline::TexturePipeline(VulkanContext& ctx) : m_ctx(ctx) {
    if (!m_ctx.isInitialized()) {
        throw std::runtime_error("TexturePipeline requires an initialized VulkanContext.");
    }
}

TexturePipeline::~TexturePipeline() = default;

TextureHandle TexturePipeline::upload(const float* data, int width, int height, int channels, VkFormat format) {
    validateTextureArgs(data, width, height, channels, format);

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) *
                                   static_cast<VkDeviceSize>(bytesPerPixel(format));

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;

    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = imageSize;
    bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocationCreateInfo{};
    stagingAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    checkVmaResult(vmaCreateBuffer(m_ctx.allocator(),
                                   &bufferCreateInfo,
                                   &stagingAllocationCreateInfo,
                                   &stagingBuffer,
                                   &stagingAllocation,
                                   nullptr),
                   "Failed to create upload staging buffer.");

    void* mappedData = nullptr;
    TextureHandle handle{};
    try {
        checkVmaResult(vmaMapMemory(m_ctx.allocator(), stagingAllocation, &mappedData),
                       "Failed to map upload staging buffer.");

        const size_t sampleCount = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
        if (isHalfFormat(format)) {
            auto* dst = static_cast<uint16_t*>(mappedData);
            for (size_t i = 0; i < sampleCount; ++i) {
                dst[i] = floatToHalf(data[i]);
            }
        } else {
            std::memcpy(mappedData, data, static_cast<size_t>(imageSize));
        }
        checkVmaResult(vmaFlushAllocation(m_ctx.allocator(), stagingAllocation, 0, imageSize),
                       "Failed to flush upload staging buffer.");
        vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        mappedData = nullptr;

        handle.format = format;
        handle.width = width;
        handle.height = height;
        handle.channels = channels;

        VkImageCreateInfo imageCreateInfo{};
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        imageCreateInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.format = format;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo imageAllocationCreateInfo{};
        imageAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        checkVmaResult(vmaCreateImage(m_ctx.allocator(),
                                      &imageCreateInfo,
                                      &imageAllocationCreateInfo,
                                      &handle.image,
                                      &handle.allocation,
                                      nullptr),
                       "Failed to create GPU texture image.");

        VkImageViewCreateInfo imageViewCreateInfo{};
        imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewCreateInfo.image = handle.image;
        imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewCreateInfo.format = format;
        imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
        imageViewCreateInfo.subresourceRange.levelCount = 1;
        imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
        imageViewCreateInfo.subresourceRange.layerCount = 1;

        checkVkResult(vkCreateImageView(m_ctx.device(), &imageViewCreateInfo, nullptr, &handle.view),
                      "Failed to create GPU texture image view.");

        VkCommandBuffer cmdBuf = beginOneTimeCommands();
        transitionImageLayout(cmdBuf, handle.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

        vkCmdCopyBufferToImage(cmdBuf,
                               stagingBuffer,
                               handle.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &copyRegion);

        transitionImageLayout(cmdBuf,
                              handle.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endOneTimeCommands(cmdBuf);

        vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        return handle;
    } catch (...) {
        if (handle.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_ctx.device(), handle.view, nullptr);
        }
        if (handle.image != VK_NULL_HANDLE || handle.allocation != nullptr) {
            vmaDestroyImage(m_ctx.allocator(), handle.image, handle.allocation);
        }
        if (mappedData != nullptr) {
            vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        }
        if (stagingBuffer != VK_NULL_HANDLE || stagingAllocation != nullptr) {
            vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        }
        throw;
    }
}

std::vector<float> TexturePipeline::download(const TextureHandle& handle) {
    if (handle.image == VK_NULL_HANDLE || handle.allocation == nullptr) {
        throw std::runtime_error("Texture download requires a valid texture handle.");
    }
    if (!isSupportedFormat(handle.format)) {
        throw std::runtime_error("Texture handle format is not supported by TexturePipeline.");
    }

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(handle.width) * static_cast<VkDeviceSize>(handle.height) *
                                   static_cast<VkDeviceSize>(bytesPerPixel(handle.format));

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;

    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = imageSize;
    bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocationCreateInfo{};
    stagingAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

    checkVmaResult(vmaCreateBuffer(m_ctx.allocator(),
                                   &bufferCreateInfo,
                                   &stagingAllocationCreateInfo,
                                   &stagingBuffer,
                                   &stagingAllocation,
                                   nullptr),
                   "Failed to create download staging buffer.");

    void* mappedData = nullptr;
    try {
        VkCommandBuffer cmdBuf = beginOneTimeCommands();
        transitionImageLayout(cmdBuf,
                              handle.image,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageOffset = {0, 0, 0};
        copyRegion.imageExtent = {static_cast<uint32_t>(handle.width), static_cast<uint32_t>(handle.height), 1};

        vkCmdCopyImageToBuffer(cmdBuf,
                               handle.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               stagingBuffer,
                               1,
                               &copyRegion);

        transitionImageLayout(cmdBuf,
                              handle.image,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endOneTimeCommands(cmdBuf);

        checkVmaResult(vmaMapMemory(m_ctx.allocator(), stagingAllocation, &mappedData),
                       "Failed to map download staging buffer.");
        checkVmaResult(vmaInvalidateAllocation(m_ctx.allocator(), stagingAllocation, 0, imageSize),
                       "Failed to invalidate download staging buffer.");

        const size_t sampleCount = static_cast<size_t>(handle.width) * static_cast<size_t>(handle.height) *
                                   static_cast<size_t>(handle.channels);
        std::vector<float> result(sampleCount, 0.0f);

        if (isHalfFormat(handle.format)) {
            const auto* src = static_cast<const uint16_t*>(mappedData);
            for (size_t i = 0; i < sampleCount; ++i) {
                result[i] = halfToFloat(src[i]);
            }
        } else {
            std::memcpy(result.data(), mappedData, sampleCount * sizeof(float));
        }

        vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        mappedData = nullptr;
        vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        return result;
    } catch (...) {
        if (mappedData != nullptr) {
            vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        }
        if (stagingBuffer != VK_NULL_HANDLE || stagingAllocation != nullptr) {
            vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        }
        throw;
    }
}

void TexturePipeline::destroy(TextureHandle& handle) {
    if (handle.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx.device(), handle.view, nullptr);
        handle.view = VK_NULL_HANDLE;
    }
    if (handle.image != VK_NULL_HANDLE || handle.allocation != nullptr) {
        vmaDestroyImage(m_ctx.allocator(), handle.image, handle.allocation);
        handle.image = VK_NULL_HANDLE;
        handle.allocation = nullptr;
    }
    handle.format = VK_FORMAT_UNDEFINED;
    handle.width = 0;
    handle.height = 0;
    handle.channels = 0;
}

VkCommandBuffer TexturePipeline::beginOneTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_ctx.commandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    checkVkResult(vkAllocateCommandBuffers(m_ctx.device(), &allocInfo, &cmdBuf),
                  "Failed to allocate one-time command buffer.");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVkResult(vkBeginCommandBuffer(cmdBuf, &beginInfo), "Failed to begin one-time command buffer.");
    return cmdBuf;
}

void TexturePipeline::endOneTimeCommands(VkCommandBuffer cmdBuf) {
    checkVkResult(vkEndCommandBuffer(cmdBuf), "Failed to end one-time command buffer.");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    checkVkResult(vkQueueSubmit(m_ctx.computeQueue(), 1, &submitInfo, VK_NULL_HANDLE),
                  "Failed to submit one-time command buffer.");
    checkVkResult(vkQueueWaitIdle(m_ctx.computeQueue()), "Failed waiting for compute queue idle.");
    vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &cmdBuf);
}

void TexturePipeline::transitionImageLayout(VkCommandBuffer cmdBuf,
                                            VkImage image,
                                            VkImageLayout oldLayout,
                                            VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    } else {
        throw std::runtime_error("Unsupported image layout transition requested.");
    }

    vkCmdPipelineBarrier(cmdBuf,
                         sourceStage,
                         destinationStage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}

uint16_t TexturePipeline::floatToHalf(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));

    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    uint32_t exponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent == 0xFFu) {
        return static_cast<uint16_t>(sign | (mantissa != 0 ? 0x7E00u : 0x7C00u));
    }

    const int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
    if (halfExponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (halfExponent <= 0) {
        if (halfExponent < -10) {
            return sign;
        }

        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
        uint16_t halfMantissa = static_cast<uint16_t>(mantissa >> shift);
        if (((mantissa >> (shift - 1)) & 1u) != 0u) {
            ++halfMantissa;
        }
        return static_cast<uint16_t>(sign | halfMantissa);
    }

    uint16_t halfMantissa = static_cast<uint16_t>(mantissa >> 13);
    if ((mantissa & 0x00001000u) != 0u) {
        ++halfMantissa;
        if ((halfMantissa & 0x0400u) != 0u) {
            halfMantissa = 0;
            if (halfExponent + 1 >= 31) {
                return static_cast<uint16_t>(sign | 0x7C00u);
            }
            return static_cast<uint16_t>(sign | static_cast<uint16_t>(halfExponent + 1) << 10 | halfMantissa);
        }
    }

    return static_cast<uint16_t>(sign | (static_cast<uint16_t>(halfExponent) << 10) | halfMantissa);
}

float TexturePipeline::halfToFloat(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x03FFu;

    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            uint32_t normalizedMantissa = mantissa;
            int32_t adjustedExponent = -14;
            while ((normalizedMantissa & 0x0400u) == 0u) {
                normalizedMantissa <<= 1;
                --adjustedExponent;
            }
            normalizedMantissa &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(adjustedExponent + 127) << 23) | (normalizedMantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
