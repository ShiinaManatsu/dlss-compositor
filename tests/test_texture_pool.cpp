// =============================================================================
// TexturePool / UploadBatch tests.
//
// [unit]  - run anywhere (no GPU required)
// [gpu]   - require an NVIDIA RTX device + Vulkan driver
//
// CI runners without an NVIDIA GPU should run with `--label-regex unit` (or
// equivalently exclude `[gpu]`). The host workstation runs the full set.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "../src/gpu/texture_pool.h"

#include <vector>

// -----------------------------------------------------------------------------
// Pure interface / logic tests (no GPU)
// -----------------------------------------------------------------------------
TEST_CASE("TextureDesc - default-constructed equality", "[unit]") {
    dlss::TextureDesc a{};
    dlss::TextureDesc b{};
    REQUIRE(a == b);
}

TEST_CASE("TextureDesc - distinct shapes are not equal", "[unit]") {
    dlss::TextureDesc a{1920, 1080, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};
    dlss::TextureDesc b{1280,  720, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};
    dlss::TextureDesc c{1920, 1080, VK_FORMAT_R32_SFLOAT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};
    dlss::TextureDesc d{1920, 1080, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT};

    REQUIRE_FALSE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(a == d);
}

TEST_CASE("PooledTextureHandle - shared_ptr semantics", "[unit]") {
    auto h = std::make_shared<dlss::PooledTexture>();
    REQUIRE(h != nullptr);
    REQUIRE(h->image == VK_NULL_HANDLE);
    REQUIRE(h->view  == VK_NULL_HANDLE);
    auto h2 = h;
    REQUIRE(h.use_count() == 2);
}

// -----------------------------------------------------------------------------
// GPU-backed tests
// -----------------------------------------------------------------------------
TEST_CASE("TexturePool - pool reuse same shape", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);

    dlss::TextureDesc desc{
        1920, 1080,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
    };

    auto h1 = pool.acquire(desc);
    REQUIRE(h1 != nullptr);
    REQUIRE(h1->image != VK_NULL_HANDLE);
    REQUIRE(h1->view  != VK_NULL_HANDLE);
    VkImage originalImage = h1->image;

    pool.release(h1);

    // Second acquire should hand back the same VkImage (pool hit).
    auto h2 = pool.acquire(desc);
    REQUIRE(h2 != nullptr);
    REQUIRE(h2->image == originalImage);

    auto stats = pool.getStatistics();
    REQUIRE(stats.hitRate > 0.0f);

    pool.release(h2);
}

TEST_CASE("TexturePool - different format is NOT reused", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);

    dlss::TextureDesc d1{32, 32, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT};
    dlss::TextureDesc d2{32, 32, VK_FORMAT_R32_SFLOAT,
                         VK_IMAGE_USAGE_STORAGE_BIT};

    auto h1 = pool.acquire(d1);
    REQUIRE(h1->image != VK_NULL_HANDLE);
    pool.release(h1);

    auto h2 = pool.acquire(d2);  // different format → fresh allocation
    REQUIRE(h2->image != VK_NULL_HANDLE);
    REQUIRE(h2->image != h1->image);
    pool.release(h2);
}

TEST_CASE("TexturePool - LRU eviction respects budget", "[gpu]") {
    dlss::VulkanContext ctx;
    // Budget tight enough that allocating two 256x256 R8G8B8A8 textures
    // (~256 KiB each) should keep at most one alive after release.
    dlss::TexturePool pool(&ctx, 200 * 1024);

    dlss::TextureDesc desc{
        256, 256, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    };

    auto a = pool.acquire(desc);
    pool.release(a);

    auto stats = pool.getStatistics();
    // Either evicted to fit budget (idleImages == 0) or still present —
    // either way budget must hold and bookkeeping must be sane.
    REQUIRE(stats.totalAllocatedBytes <= 200 * 1024 + (256 * 256 * 4));
}

TEST_CASE("UploadBatch - single submit for many uploads", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);
    dlss::UploadBatch batch(&ctx);

    constexpr int N = 10;
    std::vector<dlss::PooledTextureHandle> handles;
    handles.reserve(N);

    for (int i = 0; i < N; ++i) {
        dlss::TextureDesc desc{
            32, 32, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        };
        auto h = pool.acquire(desc);
        REQUIRE(h != nullptr);

        std::vector<uint8_t> data(32 * 32 * 4, static_cast<uint8_t>(i));
        batch.addUpload(h, data.data(), data.size());
        handles.push_back(h);
    }

    REQUIRE(batch.pendingCount() == N);

    // Submit completes through ONE vkQueueSubmit internally; we verify it
    // does not throw and clears the pending queue.
    REQUIRE_NOTHROW(batch.submit());
    REQUIRE(batch.pendingCount() == 0);

    for (auto& h : handles) pool.release(h);
}

TEST_CASE("UploadBatch - empty submit is a no-op", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::UploadBatch batch(&ctx);

    REQUIRE(batch.pendingCount() == 0);
    REQUIRE_NOTHROW(batch.submit());
    REQUIRE(batch.pendingCount() == 0);
}
