#include <catch2/catch_test_macros.hpp>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "config.h"
#include "dlss/dlss_fg_processor.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "core/camera_data_loader.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct TextureGuard {
    explicit TextureGuard(TexturePipeline& pipelineIn) : pipeline(pipelineIn) {}

    ~TextureGuard() {
        for (TextureHandle* handle : handles) {
            if (handle != nullptr) {
                pipeline.destroy(*handle);
            }
        }
    }

    void track(TextureHandle& handle) {
        handles.push_back(&handle);
    }

    TexturePipeline& pipeline;
    std::vector<TextureHandle*> handles;
};

void transitionImage(VkCommandBuffer cmdBuf,
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

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else {
        FAIL("Unsupported image layout transition in DLSS-FG test");
    }

    vkCmdPipelineBarrier(cmdBuf,
                         srcStage,
                         dstStage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}

VkCommandBuffer allocateCommandBuffer(VulkanContext& ctx) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ctx.commandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(ctx.device(), &allocInfo, &cmdBuf) == VK_SUCCESS);
    return cmdBuf;
}

void beginCommandBuffer(VkCommandBuffer cmdBuf) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmdBuf, &beginInfo) == VK_SUCCESS);
}

void submitAndWait(VulkanContext& ctx, VkCommandBuffer cmdBuf) {
    REQUIRE(vkEndCommandBuffer(cmdBuf) == VK_SUCCESS);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    REQUIRE(vkCreateFence(ctx.device(), &fenceInfo, nullptr, &fence) == VK_SUCCESS);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    REQUIRE(vkQueueSubmit(ctx.computeQueue(), 1, &submitInfo, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
    REQUIRE(vkQueueWaitIdle(ctx.computeQueue()) == VK_SUCCESS);

    vkDestroyFence(ctx.device(), fence, nullptr);
    vkFreeCommandBuffers(ctx.device(), ctx.commandPool(), 1, &cmdBuf);
}

std::vector<float> makeData(int width, int height, int channels, float seed) {
    std::vector<float> data(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) *
                                         static_cast<size_t>(channels) +
                                     static_cast<size_t>(c);
                data[index] = seed + static_cast<float>((x + y + c) % 17) / 17.0f;
            }
        }
    }
    return data;
}

} // namespace

TEST_CASE("dlss_fg_availability", "[fg]") {
    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No compatible GPU");
    }

    NgxContext ngx;
    REQUIRE(ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg));

    if (!ngx.isDlssFGAvailable()) {
        SKIP("DLSS-G not available on this GPU");
    }

    REQUIRE(ngx.maxMultiFrameCount() >= 1);
}

TEST_CASE("dlss_fg_feature_creation", "[fg]") {
    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No compatible GPU");
    }

    NgxContext ngx;
    REQUIRE(ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg));

    if (!ngx.isDlssFGAvailable()) {
        SKIP("DLSS-G not available on this GPU");
    }

    VkCommandBuffer createCmdBuf = allocateCommandBuffer(ctx);
    beginCommandBuffer(createCmdBuf);
    REQUIRE(ngx.createDlssFG(64, 64,
                              static_cast<unsigned int>(VK_FORMAT_R16G16B16A16_SFLOAT),
                              createCmdBuf, errorMsg));
    submitAndWait(ctx, createCmdBuf);

    REQUIRE(ngx.fgFeatureHandle() != nullptr);

    ngx.releaseDlssFG();
}

TEST_CASE("dlss_fg_single_evaluation", "[fg]") {
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;

    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No compatible GPU");
    }

    NgxContext ngx;
    REQUIRE(ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg));

    if (!ngx.isDlssFGAvailable()) {
        SKIP("DLSS-G not available on this GPU");
    }

    VkCommandBuffer createCmdBuf = allocateCommandBuffer(ctx);
    beginCommandBuffer(createCmdBuf);
    REQUIRE(ngx.createDlssFG(kWidth, kHeight,
                              static_cast<unsigned int>(VK_FORMAT_R16G16B16A16_SFLOAT),
                              createCmdBuf, errorMsg));
    submitAndWait(ctx, createCmdBuf);

    TexturePipeline pipeline(ctx);
    TextureGuard textureGuard(pipeline);

    const std::vector<float> backbufferData = makeData(kWidth, kHeight, 4, 0.1f);
    const std::vector<float> depthData = makeData(kWidth, kHeight, 1, 0.2f);
    const std::vector<float> motionData = makeData(kWidth, kHeight, 2, 0.01f);
    const std::vector<float> outputInit(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4u, 0.0f);

    auto backbuffer = pipeline.upload(backbufferData.data(), kWidth, kHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(backbuffer);
    auto depth = pipeline.upload(depthData.data(), kWidth, kHeight, 1, VK_FORMAT_R32_SFLOAT);
    textureGuard.track(depth);
    auto motion = pipeline.upload(motionData.data(), kWidth, kHeight, 2, VK_FORMAT_R16G16_SFLOAT);
    textureGuard.track(motion);
    auto outputInterp = pipeline.upload(outputInit.data(), kWidth, kHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(outputInterp);

    DlssFgCameraParams camParams{};
    for (int i = 0; i < 4; ++i) {
        camParams.cameraViewToClip[i][i] = 1.0f;
        camParams.clipToCameraView[i][i] = 1.0f;
        camParams.clipToLensClip[i][i] = 1.0f;
        camParams.clipToPrevClip[i][i] = 1.0f;
        camParams.prevClipToClip[i][i] = 1.0f;
    }
    camParams.cameraPos[2] = 5.0f;
    camParams.cameraUp[1] = 1.0f;
    camParams.cameraRight[0] = 1.0f;
    camParams.cameraForward[2] = -1.0f;
    camParams.nearPlane = 0.1f;
    camParams.farPlane = 100.0f;
    camParams.fov = 0.6911f;
    camParams.aspectRatio = 1.0f;

    DlssFgProcessor processor(ctx, ngx);

    DlssFgFrameInput frame{};
    frame.backbuffer = backbuffer.image;
    frame.backbufferView = backbuffer.view;
    frame.depth = depth.image;
    frame.depthView = depth.view;
    frame.motionVectors = motion.image;
    frame.motionView = motion.view;
    frame.outputInterp = outputInterp.image;
    frame.outputInterpView = outputInterp.view;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.reset = true;
    frame.cameraParams = camParams;
    frame.multiFrameCount = 1;
    frame.multiFrameIndex = 1;

    VkCommandBuffer evalCmdBuf = allocateCommandBuffer(ctx);
    beginCommandBuffer(evalCmdBuf);
    transitionImage(evalCmdBuf, outputInterp.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    REQUIRE(processor.evaluate(evalCmdBuf, frame, errorMsg));
    transitionImage(evalCmdBuf, outputInterp.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submitAndWait(ctx, evalCmdBuf);

    const std::vector<float> result = pipeline.download(outputInterp);
    REQUIRE(result.size() == static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4u);
    REQUIRE(std::any_of(result.begin(), result.end(), [](float v) { return v != 0.0f; }));
}
