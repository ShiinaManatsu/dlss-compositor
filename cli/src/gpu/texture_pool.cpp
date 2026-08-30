// =============================================================================
// TexturePool / UploadBatch implementation. See texture_pool.h for design notes.
// =============================================================================
#include "texture_pool.h"
#include "../core/log.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace dlss {

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
namespace {

uint32_t bytesPerPixel(VkFormat f) noexcept {
    switch (f) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_S8_UINT:
            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_D16_UNORM:
            return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            // Conservative fallback for unknown formats.
            return 4;
    }
}

bool isDepthFormat(VkFormat f) noexcept {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

VkImageAspectFlags aspectMaskFor(VkFormat f) noexcept {
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (isDepthFormat(f))
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace

// -----------------------------------------------------------------------------
// TexturePool
// -----------------------------------------------------------------------------
TexturePool::TexturePool(VulkanContext* ctx, uint64_t budgetBytes)
    : m_ctx(ctx), m_budgetBytes(budgetBytes) {
    if (!m_ctx)
        throw std::runtime_error("TexturePool: VulkanContext must not be null");
}

TexturePool::~TexturePool() {
    // Drain everything still tracked. shared_ptrs in m_free / m_active hold the
    // last reference, so resetting them triggers our explicit destroyResources
    // path through the destructor of each PooledTexture? No — PooledTexture has
    // no destructor. We must walk both lists and destroy resources manually.
    std::lock_guard<std::mutex> lock(m_mutex);

    auto destroyAll = [this](std::vector<PooledTextureHandle>& v) {
        for (auto& h : v) {
            if (h) destroyResources(*h);
        }
        v.clear();
    };

    for (auto& [_, vec] : m_free) {
        destroyAll(vec);
    }
    m_free.clear();

    for (auto& h : m_active) {
        if (h) destroyResources(*h);
    }
    m_active.clear();

    m_totalBytes = 0;
}

TexturePool::PoolKey TexturePool::makeKey(const TextureDesc& d) {
    return PoolKey{
        d.width,
        d.height,
        static_cast<uint32_t>(d.format),
        static_cast<uint32_t>(d.usage)};
}

uint64_t TexturePool::estimateSizeBytes(const TextureDesc& desc) const {
    const uint64_t bpp = bytesPerPixel(desc.format);
    return static_cast<uint64_t>(desc.width) * desc.height * bpp;
}

PooledTextureHandle TexturePool::acquire(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0)
        throw std::runtime_error("TexturePool::acquire: zero-sized texture");

    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_acquireCount;

    const PoolKey key = makeKey(desc);
    auto it = m_free.find(key);
    if (it != m_free.end() && !it->second.empty()) {
        // Pool hit: pop the most recently freed texture (back of vector).
        PooledTextureHandle h = std::move(it->second.back());
        it->second.pop_back();
        ++m_hitCount;
        h->lastUsed = ++m_ticker;
        m_active.push_back(h);
        return h;
    }

    // Miss: allocate a fresh texture.
    PooledTextureHandle h = allocateNew(desc);
    h->lastUsed = ++m_ticker;
    m_active.push_back(h);
    return h;
}

void TexturePool::release(PooledTextureHandle handle) {
    if (!handle) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Remove from active list (linear scan is fine — m_active is small,
    // bounded by working set size, typically <100).
    m_active.remove(handle);

    handle->lastUsed = ++m_ticker;
    const PoolKey key = makeKey(handle->desc);
    m_free[key].push_back(std::move(handle));

    // Enforce budget by evicting oldest free textures.
    while (m_totalBytes > m_budgetBytes) {
        const auto totalBefore = m_totalBytes;
        evictLRU();
        if (m_totalBytes == totalBefore) break;  // nothing left to evict
    }
}

PooledTextureHandle TexturePool::allocateNew(const TextureDesc& desc) {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = desc.format;
    imgInfo.extent        = { desc.width, desc.height, 1 };
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = desc.usage;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    auto t = std::make_shared<PooledTexture>();
    t->desc      = desc;
    t->sizeBytes = estimateSizeBytes(desc);

    VkResult r = vmaCreateImage(m_ctx->allocator(), &imgInfo, &allocInfo,
                                &t->image, &t->allocation, nullptr);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("TexturePool: vmaCreateImage failed: "
                                 + std::to_string(r));
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = t->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = desc.format;
    viewInfo.subresourceRange.aspectMask     = aspectMaskFor(desc.format);
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    r = vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &t->view);
    if (r != VK_SUCCESS) {
        vmaDestroyImage(m_ctx->allocator(), t->image, t->allocation);
        t->image      = VK_NULL_HANDLE;
        t->allocation = VK_NULL_HANDLE;
        throw std::runtime_error("TexturePool: vkCreateImageView failed: "
                                 + std::to_string(r));
    }

    m_totalBytes += t->sizeBytes;
    return t;
}

void TexturePool::evictLRU() {
    // Find the oldest (smallest lastUsed) PooledTexture across all free buckets.
    PoolKey  oldestKey{};
    uint64_t oldestTick = UINT64_MAX;
    size_t   oldestIdx  = 0;
    bool     found      = false;

    for (auto& [key, vec] : m_free) {
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i] && vec[i]->lastUsed < oldestTick) {
                oldestTick = vec[i]->lastUsed;
                oldestKey  = key;
                oldestIdx  = i;
                found      = true;
            }
        }
    }
    if (!found) return;

    auto& vec = m_free[oldestKey];
    PooledTextureHandle victim = std::move(vec[oldestIdx]);
    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(oldestIdx));
    if (vec.empty()) m_free.erase(oldestKey);

    m_totalBytes = (victim->sizeBytes <= m_totalBytes)
                       ? m_totalBytes - victim->sizeBytes
                       : 0;
    destroyResources(*victim);
    // shared_ptr drops here — refcount must be 1 (only m_free held it).
}

void TexturePool::destroyResources(PooledTexture& t) {
    if (t.view) {
        vkDestroyImageView(m_ctx->device(), t.view, nullptr);
        t.view = VK_NULL_HANDLE;
    }
    if (t.image && t.allocation) {
        vmaDestroyImage(m_ctx->allocator(), t.image, t.allocation);
        t.image      = VK_NULL_HANDLE;
        t.allocation = VK_NULL_HANDLE;
    }
}

PoolStatistics TexturePool::getStatistics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    PoolStatistics s{};
    s.totalAllocatedBytes = m_totalBytes;
    s.totalImages         = m_active.size();
    for (const auto& [_, vec] : m_free) {
        s.idleImages += vec.size();
        for (const auto& h : vec) {
            if (h) s.idlePoolBytes += h->sizeBytes;
        }
    }
    s.totalImages += s.idleImages;
    s.hitRate = (m_acquireCount > 0)
                    ? static_cast<float>(m_hitCount) / static_cast<float>(m_acquireCount)
                    : 0.0f;
    return s;
}

// -----------------------------------------------------------------------------
// UploadBatch
// -----------------------------------------------------------------------------
UploadBatch::UploadBatch(VulkanContext* ctx) : m_ctx(ctx) {
    if (!m_ctx)
        throw std::runtime_error("UploadBatch: VulkanContext must not be null");
}

UploadBatch::~UploadBatch() {
    // If the user forgot to submit, we still must release staging GPU memory.
    freeStagingAll();
}

void UploadBatch::addUpload(PooledTextureHandle dst, const void* srcData, size_t dataSize,
                            VkImageLayout finalLayout) {
    if (!dst)             throw std::runtime_error("UploadBatch::addUpload: null destination");
    if (!srcData)         throw std::runtime_error("UploadBatch::addUpload: null source");
    if (dataSize == 0)    throw std::runtime_error("UploadBatch::addUpload: zero size");

    UploadJob job;
    job.dst         = std::move(dst);
    job.dataSize    = dataSize;
    job.finalLayout = finalLayout;
    job.data.resize(dataSize);
    std::memcpy(job.data.data(), srcData, dataSize);
    m_uploads.push_back(std::move(job));
}

UploadBatch::StagingBuffer UploadBatch::allocStaging(size_t size) {
    StagingBuffer sb{};
    sb.size = size;

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(m_ctx->allocator(), &bi, &ai,
                                 &sb.buffer, &sb.allocation, &info);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("UploadBatch: vmaCreateBuffer (staging) failed: "
                                 + std::to_string(r));
    }
    sb.mapped = info.pMappedData;
    return sb;
}

void UploadBatch::freeStagingAll() {
    for (auto& sb : m_stagingBuffers) {
        if (sb.buffer && sb.allocation) {
            vmaDestroyBuffer(m_ctx->allocator(), sb.buffer, sb.allocation);
        }
    }
    m_stagingBuffers.clear();
}

void UploadBatch::submit() {
    if (m_uploads.empty()) return;

    // ------------------------------------------------------------------
    // 1. Allocate staging buffers and copy CPU data in.
    // ------------------------------------------------------------------
    m_stagingBuffers.reserve(m_uploads.size());
    for (auto& job : m_uploads) {
        StagingBuffer sb = allocStaging(job.dataSize);
        if (!sb.mapped) {
            throw std::runtime_error("UploadBatch: staging buffer mapping is null");
        }
        std::memcpy(sb.mapped, job.data.data(), job.dataSize);
        m_stagingBuffers.push_back(sb);
    }

    // ------------------------------------------------------------------
    // 2. Build ONE command buffer with all copy + barrier work.
    // ------------------------------------------------------------------
    VkCommandPool pool = m_ctx->createComputeCommandPool();
    if (pool == VK_NULL_HANDLE)
        throw std::runtime_error("UploadBatch: failed to create command pool");

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_ctx->device(), &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
        throw std::runtime_error("UploadBatch: vkAllocateCommandBuffers failed");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    for (size_t i = 0; i < m_uploads.size(); ++i) {
        const auto& job = m_uploads[i];
        const auto& sb  = m_stagingBuffers[i];
        const auto& d   = job.dst->desc;
        const VkImageAspectFlags aspect = aspectMaskFor(d.format);

        // ---- barrier: UNDEFINED -> TRANSFER_DST_OPTIMAL ----
        VkImageMemoryBarrier2 toTransfer{};
        toTransfer.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toTransfer.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
        toTransfer.srcAccessMask    = 0;
        toTransfer.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
        toTransfer.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image            = job.dst->image;
        toTransfer.subresourceRange.aspectMask     = aspect;
        toTransfer.subresourceRange.baseMipLevel   = 0;
        toTransfer.subresourceRange.levelCount     = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount     = 1;

        VkDependencyInfo dep1{};
        dep1.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep1.imageMemoryBarrierCount = 1;
        dep1.pImageMemoryBarriers    = &toTransfer;
        vkCmdPipelineBarrier2(cmd, &dep1);

        // ---- copy ----
        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = aspect;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { d.width, d.height, 1 };

        vkCmdCopyBufferToImage(cmd, sb.buffer, job.dst->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

        // ---- barrier: TRANSFER_DST_OPTIMAL -> finalLayout (compute readable) ----
        VkImageMemoryBarrier2 toFinal{};
        toFinal.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toFinal.srcStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
        toFinal.srcAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toFinal.dstStageMask     = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                 | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toFinal.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT
                                 | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        toFinal.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toFinal.newLayout        = job.finalLayout;
        toFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.image            = job.dst->image;
        toFinal.subresourceRange.aspectMask     = aspect;
        toFinal.subresourceRange.baseMipLevel   = 0;
        toFinal.subresourceRange.levelCount     = 1;
        toFinal.subresourceRange.baseArrayLayer = 0;
        toFinal.subresourceRange.layerCount     = 1;

        VkDependencyInfo dep2{};
        dep2.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers    = &toFinal;
        vkCmdPipelineBarrier2(cmd, &dep2);
    }

    vkEndCommandBuffer(cmd);

    // ------------------------------------------------------------------
    // 3. SINGLE vkQueueSubmit for the whole batch.
    // ------------------------------------------------------------------
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    VkQueue queue = m_ctx->computeQueue();
    VkResult r = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(m_ctx->device(), pool, 1, &cmd);
        vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
        freeStagingAll();
        m_uploads.clear();
        throw std::runtime_error("UploadBatch: vkQueueSubmit failed: " + std::to_string(r));
    }
    r = vkQueueWaitIdle(queue);  // staging buffers are freed below — must be safe to do so
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(m_ctx->device(), pool, 1, &cmd);
        vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
        freeStagingAll();
        m_uploads.clear();
        throw std::runtime_error("UploadBatch: vkQueueWaitIdle failed: " + std::to_string(r));
    }

    // ------------------------------------------------------------------
    // 4. Cleanup. Staging is no longer needed; command buffer/pool are
    //    one-shot for this batch.
    // ------------------------------------------------------------------
    vkFreeCommandBuffers(m_ctx->device(), pool, 1, &cmd);
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);

    freeStagingAll();
    m_uploads.clear();
}

} // namespace dlss
