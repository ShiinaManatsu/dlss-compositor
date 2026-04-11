#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "gpu/texture_pipeline.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class VulkanContext;

class TexturePool {
public:
    explicit TexturePool(VulkanContext& ctx, TexturePipeline& pipeline, int64_t maxMemoryBytes);
    ~TexturePool();

    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    // Acquire a texture handle from the pool (reuses cached, or creates new)
    TextureHandle acquire(int width, int height, int channels, VkFormat format);

    // Update pixel data in an existing handle (no VkImage recreation)
    void updateData(TextureHandle& handle, const float* data);

    // Release handle back to pool (does NOT destroy, marks as reusable)
    void release(TextureHandle& handle);

    // Destroy all pooled resources
    void releaseAll();

    // Current GPU memory usage in bytes
    int64_t currentMemoryBytes() const;

private:
    struct PoolKey {
        int width;
        int height;
        int channels;
        VkFormat format;
        bool operator==(const PoolKey& other) const;
    };

    struct PoolKeyHash {
        size_t operator()(const PoolKey& key) const;
    };

    int64_t estimateBytes(int width, int height, VkFormat format) const;
    void evictIfNeeded(int64_t requiredBytes);

    VulkanContext& m_ctx;
    TexturePipeline& m_pipeline;
    int64_t m_maxMemoryBytes;
    int64_t m_currentMemoryBytes = 0;
    std::unordered_map<PoolKey, std::vector<TextureHandle>, PoolKeyHash> m_available;
    std::vector<TextureHandle> m_inUse;
};
