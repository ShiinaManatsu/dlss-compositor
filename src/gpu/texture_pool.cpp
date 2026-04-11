#include "gpu/texture_pool.h"

#include "gpu/vulkan_context.h"

#include <vk_mem_alloc.h>

#include <algorithm>
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

void validatePoolArgs(int width, int height, int channels, VkFormat format) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Texture dimensions must be positive.");
    }
    if (!isSupportedFormat(format)) {
        throw std::runtime_error("Texture format is not supported by TexturePool.");
    }
    if (channels != static_cast<int>(expectedChannels(format))) {
        throw std::runtime_error("Texture channel count does not match the requested Vulkan format.");
    }
}

void validateTextureArgs(const float* data, int width, int height, int channels, VkFormat format) {
    if (data == nullptr) {
        throw std::runtime_error("TexturePool update received null data pointer.");
    }
    validatePoolArgs(width, height, channels, format);
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

uint16_t floatToHalf(float f) {
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

void transitionImageLayout(VkCommandBuffer cmdBuf, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
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
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
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

} // namespace

TexturePool::TexturePool(VulkanContext& ctx, TexturePipeline& pipeline, int64_t maxMemoryBytes)
    : m_ctx(ctx), m_pipeline(pipeline), m_maxMemoryBytes(maxMemoryBytes) {
    if (!m_ctx.isInitialized()) {
        throw std::runtime_error("TexturePool requires an initialized VulkanContext.");
    }
}

TexturePool::~TexturePool() {
    releaseAll();
}

TextureHandle TexturePool::acquire(int width, int height, int channels, VkFormat format) {
    validatePoolArgs(width, height, channels, format);

    PoolKey key{};
    key.width = width;
    key.height = height;
    key.channels = channels;
    key.format = format;

    std::vector<TextureHandle>& available = m_available[key];
    if (!available.empty()) {
        TextureHandle handle = available.back();
        available.pop_back();
        m_inUse.push_back(handle);
        return handle;
    }

    const int64_t requiredBytes = estimateBytes(width, height, format);
    evictIfNeeded(requiredBytes);

    const size_t sampleCount = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
    std::vector<float> zeroData(sampleCount, 0.0f);
    TextureHandle handle = m_pipeline.upload(zeroData.data(), width, height, channels, format);
    m_currentMemoryBytes += requiredBytes;
    m_inUse.push_back(handle);
    return handle;
}

void TexturePool::updateData(TextureHandle& handle, const float* data) {
    if (handle.image == VK_NULL_HANDLE || handle.view == VK_NULL_HANDLE || handle.allocation == nullptr) {
        throw std::runtime_error("TexturePool update requires a valid texture handle.");
    }

    validateTextureArgs(data, handle.width, handle.height, handle.channels, handle.format);

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(handle.width) * static_cast<VkDeviceSize>(handle.height) *
                                   static_cast<VkDeviceSize>(bytesPerPixel(handle.format));

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;

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
                   "Failed to create update staging buffer.");

    void* mappedData = nullptr;
    try {
        checkVmaResult(vmaMapMemory(m_ctx.allocator(), stagingAllocation, &mappedData),
                       "Failed to map update staging buffer.");

        const size_t sampleCount = static_cast<size_t>(handle.width) * static_cast<size_t>(handle.height) *
                                   static_cast<size_t>(handle.channels);
        if (isHalfFormat(handle.format)) {
            uint16_t* dst = static_cast<uint16_t*>(mappedData);
            for (size_t i = 0; i < sampleCount; ++i) {
                dst[i] = floatToHalf(data[i]);
            }
        } else {
            std::memcpy(mappedData, data, static_cast<size_t>(imageSize));
        }

        checkVmaResult(vmaFlushAllocation(m_ctx.allocator(), stagingAllocation, 0, imageSize),
                       "Failed to flush update staging buffer.");
        vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        mappedData = nullptr;

        cmdBuf = beginOneTimeCommands(m_ctx);
        transitionImageLayout(cmdBuf,
                              handle.image,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

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
        endOneTimeCommands(m_ctx, cmdBuf);
        cmdBuf = VK_NULL_HANDLE;

        vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
    } catch (...) {
        if (mappedData != nullptr) {
            vmaUnmapMemory(m_ctx.allocator(), stagingAllocation);
        }
        if (cmdBuf != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &cmdBuf);
        }
        if (stagingBuffer != VK_NULL_HANDLE || stagingAllocation != nullptr) {
            vmaDestroyBuffer(m_ctx.allocator(), stagingBuffer, stagingAllocation);
        }
        throw;
    }
}

void TexturePool::release(TextureHandle& handle) {
    if (handle.image == VK_NULL_HANDLE) {
        return;
    }

    std::vector<TextureHandle>::iterator it = std::find_if(m_inUse.begin(),
                                                            m_inUse.end(),
                                                            [&handle](const TextureHandle& candidate) {
                                                                return candidate.image == handle.image;
                                                            });
    if (it == m_inUse.end()) {
        return;
    }

    TextureHandle pooledHandle = *it;
    m_inUse.erase(it);

    PoolKey key{};
    key.width = pooledHandle.width;
    key.height = pooledHandle.height;
    key.channels = pooledHandle.channels;
    key.format = pooledHandle.format;
    m_available[key].push_back(pooledHandle);
    handle = TextureHandle{};
}

void TexturePool::releaseAll() {
    for (std::unordered_map<PoolKey, std::vector<TextureHandle>, PoolKeyHash>::iterator it = m_available.begin();
         it != m_available.end();
         ++it) {
        std::vector<TextureHandle>& handles = it->second;
        for (size_t i = 0; i < handles.size(); ++i) {
            m_pipeline.destroy(handles[i]);
        }
    }

    for (size_t i = 0; i < m_inUse.size(); ++i) {
        m_pipeline.destroy(m_inUse[i]);
    }

    m_available.clear();
    m_inUse.clear();
    m_currentMemoryBytes = 0;
}

int64_t TexturePool::currentMemoryBytes() const {
    return m_currentMemoryBytes;
}

bool TexturePool::PoolKey::operator==(const PoolKey& other) const {
    return width == other.width && height == other.height && channels == other.channels && format == other.format;
}

size_t TexturePool::PoolKeyHash::operator()(const PoolKey& key) const {
    const size_t h1 = std::hash<int>()(key.width);
    const size_t h2 = std::hash<int>()(key.height);
    const size_t h3 = std::hash<int>()(key.channels);
    const size_t h4 = std::hash<int>()(static_cast<int>(key.format));
    return (((h1 * 1315423911u) ^ h2) * 1315423911u ^ h3) * 1315423911u ^ h4;
}

int64_t TexturePool::estimateBytes(int width, int height, VkFormat format) const {
    const uint32_t pixelBytes = bytesPerPixel(format);
    if (pixelBytes == 0) {
        throw std::runtime_error("TexturePool cannot estimate memory for unsupported format.");
    }

    return static_cast<int64_t>(width) * static_cast<int64_t>(height) * static_cast<int64_t>(pixelBytes);
}

void TexturePool::evictIfNeeded(int64_t requiredBytes) {
    while (m_currentMemoryBytes + requiredBytes > m_maxMemoryBytes && !m_available.empty()) {
        std::unordered_map<PoolKey, std::vector<TextureHandle>, PoolKeyHash>::iterator entry = m_available.begin();
        std::vector<TextureHandle>& handles = entry->second;

        for (size_t i = 0; i < handles.size(); ++i) {
            m_currentMemoryBytes -= estimateBytes(handles[i].width, handles[i].height, handles[i].format);
            m_pipeline.destroy(handles[i]);
        }

        m_available.erase(entry);
    }
}
