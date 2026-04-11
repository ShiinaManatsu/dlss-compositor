#include <catch2/catch_test_macros.hpp>

#include "gpu/texture_pool.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"

#include <cmath>
#include <string>
#include <vector>

TEST_CASE("texture_pool_acquire_and_release", "[gpu][texture_pool]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);
    TexturePool pool(ctx, pipeline, 1024 * 1024 * 1024);

    auto h1 = pool.acquire(64, 64, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    REQUIRE(h1.image != VK_NULL_HANDLE);
    REQUIRE(h1.view != VK_NULL_HANDLE);

    VkImage cachedImage = h1.image;
    pool.release(h1);

    auto h2 = pool.acquire(64, 64, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    REQUIRE(h2.image == cachedImage);

    pool.releaseAll();
    ctx.destroy();
}

TEST_CASE("texture_pool_update_data_roundtrip", "[gpu][texture_pool]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);
    TexturePool pool(ctx, pipeline, 1024 * 1024 * 1024);

    auto handle = pool.acquire(64, 64, 4, VK_FORMAT_R16G16B16A16_SFLOAT);

    std::vector<float> testData(64 * 64 * 4);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<float>(i % 100) / 100.0f;
    }

    pool.updateData(handle, testData.data());

    const auto result = pipeline.download(handle);
    REQUIRE(result.size() == testData.size());
    for (size_t i = 0; i < testData.size(); ++i) {
        REQUIRE(std::fabs(result[i] - testData[i]) < 1e-3f);
    }

    std::vector<float> secondData(64 * 64 * 4);
    for (size_t i = 0; i < secondData.size(); ++i) {
        secondData[i] = static_cast<float>(i % 50) / 50.0f;
    }

    pool.updateData(handle, secondData.data());

    const auto result2 = pipeline.download(handle);
    REQUIRE(result2.size() == secondData.size());
    for (size_t i = 0; i < secondData.size(); ++i) {
        REQUIRE(std::fabs(result2[i] - secondData[i]) < 1e-3f);
    }

    pool.releaseAll();
    ctx.destroy();
}

TEST_CASE("texture_pool_release_all", "[gpu][texture_pool]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);
    TexturePool pool(ctx, pipeline, 1024 * 1024 * 1024);

    auto h1 = pool.acquire(64, 64, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    auto h2 = pool.acquire(32, 32, 1, VK_FORMAT_R32_SFLOAT);
    auto h3 = pool.acquire(128, 128, 2, VK_FORMAT_R16G16_SFLOAT);

    REQUIRE(pool.currentMemoryBytes() > 0);

    pool.releaseAll();

    REQUIRE(pool.currentMemoryBytes() == 0);

    ctx.destroy();
}

TEST_CASE("texture_pool_memory_budget", "[gpu][texture_pool]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);
    TexturePool pool(ctx, pipeline, 1024 * 1024);

    auto h1 = pool.acquire(64, 64, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    pool.release(h1);

    auto memBefore = pool.currentMemoryBytes();
    (void)memBefore;

    auto h2 = pool.acquire(256, 256, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    REQUIRE(pool.currentMemoryBytes() <= 1024 * 1024);

    pool.releaseAll();
    REQUIRE(pool.currentMemoryBytes() == 0);

    ctx.destroy();
}
