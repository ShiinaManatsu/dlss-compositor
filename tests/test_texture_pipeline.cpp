#include <catch2/catch_test_macros.hpp>

#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"

#include <cmath>
#include <string>
#include <vector>

TEST_CASE("roundtrip_upload_download", "[gpu][texture_pipeline]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);

    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kChannels = 4;
    std::vector<float> testData(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * static_cast<size_t>(kChannels));
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<float>(i % 100) / 100.0f;
    }

    auto handle = pipeline.upload(testData.data(), kWidth, kHeight, kChannels, VK_FORMAT_R16G16B16A16_SFLOAT);
    REQUIRE(handle.image != VK_NULL_HANDLE);
    REQUIRE(handle.view != VK_NULL_HANDLE);

    const auto result = pipeline.download(handle);
    REQUIRE(result.size() == testData.size());

    for (size_t i = 0; i < testData.size(); ++i) {
        REQUIRE(std::fabs(result[i] - testData[i]) < 1e-3f);
    }

    pipeline.destroy(handle);
    ctx.destroy();
}

TEST_CASE("batch_upload_download_roundtrip", "[gpu][texture_pipeline]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);

    constexpr int kWidth = 64;
    constexpr int kHeight = 64;

    // RGBA — 4 channels, R16G16B16A16_SFLOAT
    constexpr int kRgbaChannels = 4;
    std::vector<float> rgbaData(static_cast<size_t>(kWidth) * kHeight * kRgbaChannels);
    for (size_t i = 0; i < rgbaData.size(); ++i) {
        rgbaData[i] = static_cast<float>(i % 100) / 100.0f;
    }

    // Depth — 1 channel, R32_SFLOAT
    constexpr int kDepthChannels = 1;
    std::vector<float> depthData(static_cast<size_t>(kWidth) * kHeight * kDepthChannels);
    for (size_t i = 0; i < depthData.size(); ++i) {
        depthData[i] = static_cast<float>(i % 50) / 50.0f;
    }

    // Motion — 2 channels, R16G16_SFLOAT
    constexpr int kMotionChannels = 2;
    std::vector<float> motionData(static_cast<size_t>(kWidth) * kHeight * kMotionChannels);
    for (size_t i = 0; i < motionData.size(); ++i) {
        motionData[i] = static_cast<float>(i % 30) / 30.0f;
    }

    std::vector<TextureUploadRequest> requests = {
        { rgbaData.data(),   kWidth, kHeight, kRgbaChannels,   VK_FORMAT_R16G16B16A16_SFLOAT },
        { depthData.data(),  kWidth, kHeight, kDepthChannels,  VK_FORMAT_R32_SFLOAT },
        { motionData.data(), kWidth, kHeight, kMotionChannels, VK_FORMAT_R16G16_SFLOAT },
    };

    auto handles = pipeline.uploadBatch(requests);
    REQUIRE(handles.size() == 3);

    for (auto& h : handles) {
        REQUIRE(h.image != VK_NULL_HANDLE);
        REQUIRE(h.view != VK_NULL_HANDLE);
    }

    // Verify RGBA roundtrip (half-float tolerance)
    {
        const auto result = pipeline.download(handles[0]);
        REQUIRE(result.size() == rgbaData.size());
        for (size_t i = 0; i < rgbaData.size(); ++i) {
            REQUIRE(std::fabs(result[i] - rgbaData[i]) < 1e-3f);
        }
    }

    // Verify depth roundtrip (R32 — exact)
    {
        const auto result = pipeline.download(handles[1]);
        REQUIRE(result.size() == depthData.size());
        for (size_t i = 0; i < depthData.size(); ++i) {
            REQUIRE(result[i] == depthData[i]);
        }
    }

    // Verify motion roundtrip (half-float tolerance)
    {
        const auto result = pipeline.download(handles[2]);
        REQUIRE(result.size() == motionData.size());
        for (size_t i = 0; i < motionData.size(); ++i) {
            REQUIRE(std::fabs(result[i] - motionData[i]) < 1e-3f);
        }
    }

    for (auto& h : handles) {
        pipeline.destroy(h);
    }
    ctx.destroy();
}

TEST_CASE("batch_upload_empty_request", "[gpu][texture_pipeline]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);

    auto handles = pipeline.uploadBatch({});
    REQUIRE(handles.empty());

    ctx.destroy();
}

TEST_CASE("batch_upload_single_request", "[gpu][texture_pipeline]") {
    VulkanContext ctx;
    std::string err;
    if (!ctx.init(err)) {
        SKIP(err);
    }

    TexturePipeline pipeline(ctx);

    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kChannels = 4;
    std::vector<float> testData(static_cast<size_t>(kWidth) * kHeight * kChannels);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<float>(i % 100) / 100.0f;
    }

    std::vector<TextureUploadRequest> requests = {
        { testData.data(), kWidth, kHeight, kChannels, VK_FORMAT_R16G16B16A16_SFLOAT },
    };

    auto handles = pipeline.uploadBatch(requests);
    REQUIRE(handles.size() == 1);
    REQUIRE(handles[0].image != VK_NULL_HANDLE);
    REQUIRE(handles[0].view != VK_NULL_HANDLE);

    const auto result = pipeline.download(handles[0]);
    REQUIRE(result.size() == testData.size());
    for (size_t i = 0; i < testData.size(); ++i) {
        REQUIRE(std::fabs(result[i] - testData[i]) < 1e-3f);
    }

    pipeline.destroy(handles[0]);
    ctx.destroy();
}
