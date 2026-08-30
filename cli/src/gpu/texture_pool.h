// =============================================================================
// TexturePool — GPU texture pool with LRU eviction and UploadBatch helper.
//
// PROBLEM (v1):
//   The previous compositor called vkCreateImage / vkDestroyImage every frame
//   and issued a separate vkQueueSubmit for every CPU→GPU upload. On a 4K
//   pipeline with ~6 textures/frame this dominated the frame budget.
//
// SOLUTION (v2):
//   - TexturePool: keep textures alive after the frame is done; reuse same-shape
//     textures via a hash on (width, height, format, usage). LRU evicts when
//     the live byte count exceeds a configurable budget.
//   - UploadBatch: queue an arbitrary number of CPU→GPU uploads, then flush
//     them through ONE command buffer + ONE vkQueueSubmit. Sync2 barriers are
//     used to transition each image into the requested final layout.
//
// THREAD SAFETY:
//   TexturePool::acquire / release are mutex-guarded so multiple worker threads
//   can prepare frames in parallel. UploadBatch is single-threaded by design.
// =============================================================================
#pragma once
#include "vulkan_context.h"
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>
#include <list>
#include <unordered_map>

namespace dlss {

struct TextureDesc {
    uint32_t           width  = 0;
    uint32_t           height = 0;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags  usage  = 0;

    bool operator==(const TextureDesc&) const = default;
};

struct PooledTexture {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    TextureDesc   desc{};
    uint64_t      sizeBytes  = 0;
    uint64_t      lastUsed   = 0;  // logical tick from TexturePool, used for LRU
};

using PooledTextureHandle = std::shared_ptr<PooledTexture>;

struct PoolStatistics {
    uint64_t totalAllocatedBytes = 0;
    uint64_t idlePoolBytes       = 0;
    size_t   totalImages         = 0;
    size_t   idleImages          = 0;
    float    hitRate             = 0.0f;  // cache hit rate (0..1)
};

/// GPU texture pool with LRU eviction.
/// Prevents per-frame vkCreateImage/vkDestroyImage by reusing same-shape textures.
class TexturePool {
public:
    explicit TexturePool(VulkanContext* ctx, uint64_t budgetBytes = 4ULL * 1024 * 1024 * 1024);
    ~TexturePool();

    TexturePool(const TexturePool&)            = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    /// Acquire a texture of given desc. Returns from pool if available, else allocates.
    PooledTextureHandle acquire(const TextureDesc& desc);

    /// Return a texture to the pool (keeps it alive for reuse).
    void release(PooledTextureHandle handle);

    PoolStatistics getStatistics() const;
    void           setBudget(uint64_t bytes) { m_budgetBytes = bytes; }

private:
    struct PoolKey {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t usage;
        bool operator==(const PoolKey&) const = default;
    };
    struct PoolKeyHash {
        size_t operator()(const PoolKey& k) const noexcept {
            size_t h = std::hash<uint32_t>{}(k.width);
            h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.format) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.usage)  + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    static PoolKey makeKey(const TextureDesc& d);

    PooledTextureHandle allocateNew(const TextureDesc& desc);
    void                evictLRU();
    void                destroyResources(PooledTexture& t);
    uint64_t            estimateSizeBytes(const TextureDesc& desc) const;

    VulkanContext*  m_ctx;
    uint64_t        m_budgetBytes;
    uint64_t        m_ticker = 0;

    mutable std::mutex m_mutex;
    std::unordered_map<PoolKey, std::vector<PooledTextureHandle>, PoolKeyHash> m_free;
    std::list<PooledTextureHandle> m_active;
    uint64_t m_totalBytes    = 0;
    uint64_t m_acquireCount  = 0;
    uint64_t m_hitCount      = 0;
};

/// Batches GPU texture uploads into a single command buffer + single vkQueueSubmit.
/// Prevents the per-upload submit bottleneck from v1.
class UploadBatch {
public:
    explicit UploadBatch(VulkanContext* ctx);
    ~UploadBatch();

    UploadBatch(const UploadBatch&)            = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    /// Add an upload job: copy CPU data to GPU image.
    /// `srcData` is copied into a private staging buffer immediately, so the
    /// caller may release `srcData` as soon as addUpload returns.
    void addUpload(PooledTextureHandle dst, const void* srcData, size_t dataSize,
                   VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    /// Execute all queued uploads in a SINGLE command buffer + SINGLE vkQueueSubmit.
    /// Blocks until the queue is idle, then frees staging buffers.
    void submit();

    size_t pendingCount() const { return m_uploads.size(); }

private:
    struct UploadJob {
        PooledTextureHandle  dst;
        std::vector<uint8_t> data;
        size_t               dataSize;
        VkImageLayout        finalLayout;
    };

    struct StagingBuffer {
        VkBuffer      buffer     = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void*         mapped     = nullptr;
        size_t        size       = 0;
    };

    StagingBuffer allocStaging(size_t size);
    void          freeStagingAll();

    VulkanContext*             m_ctx;
    std::vector<UploadJob>     m_uploads;
    std::vector<StagingBuffer> m_stagingBuffers;
};

} // namespace dlss
