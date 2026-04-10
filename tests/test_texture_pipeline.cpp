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
