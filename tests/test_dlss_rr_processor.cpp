#include <catch2/catch_test_macros.hpp>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "config.h"
#include "dlss/dlss_sr_processor.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"

#include <algorithm>
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
        FAIL("Unsupported image layout transition in DLSS-RR test");
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

TEST_CASE("dlss_rr_single_evaluation", "[gpu][dlss]") {
    constexpr int kInputWidth = 64;
    constexpr int kInputHeight = 64;
    constexpr int kOutputWidth = 128;
    constexpr int kOutputHeight = 128;

    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No compatible GPU");
    }

    NgxContext ngx;
    REQUIRE(ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg));
    if (!ngx.isDlssSRAvailable()) {
        SKIP("DLSS-SR not available");
    }

    VkCommandBuffer createCmdBuf = allocateCommandBuffer(ctx);
    beginCommandBuffer(createCmdBuf);
    REQUIRE(ngx.createDlssSR(kInputWidth,
                             kInputHeight,
                             kOutputWidth,
                             kOutputHeight,
                             DlssQualityMode::Balanced,
                             DlssSRPreset::L,
                             createCmdBuf,
                             errorMsg));
    submitAndWait(ctx, createCmdBuf);

    TexturePipeline pipeline(ctx);
    TextureGuard textureGuard(pipeline);

    const std::vector<float> colorData = makeData(kInputWidth, kInputHeight, 4, 0.1f);
    const std::vector<float> depthData = makeData(kInputWidth, kInputHeight, 1, 0.2f);
    const std::vector<float> motionData = makeData(kInputWidth, kInputHeight, 2, 0.01f);
    const std::vector<float> diffuseData = makeData(kInputWidth, kInputHeight, 4, 0.3f);
    const std::vector<float> normalsData = makeData(kInputWidth, kInputHeight, 4, 0.4f);
    const std::vector<float> roughnessData = makeData(kInputWidth, kInputHeight, 1, 0.5f);
    const std::vector<float> outputInit(static_cast<size_t>(kOutputWidth) * static_cast<size_t>(kOutputHeight) * 4u, 0.0f);

    auto color = pipeline.upload(colorData.data(), kInputWidth, kInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(color);
    auto depth = pipeline.upload(depthData.data(), kInputWidth, kInputHeight, 1, VK_FORMAT_R32_SFLOAT);
    textureGuard.track(depth);
    auto motion = pipeline.upload(motionData.data(), kInputWidth, kInputHeight, 2, VK_FORMAT_R16G16_SFLOAT);
    textureGuard.track(motion);
    auto diffuse = pipeline.upload(diffuseData.data(), kInputWidth, kInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(diffuse);
    auto normals = pipeline.upload(normalsData.data(), kInputWidth, kInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(normals);
    auto roughness = pipeline.upload(roughnessData.data(), kInputWidth, kInputHeight, 1, VK_FORMAT_R32_SFLOAT);
    textureGuard.track(roughness);
    auto output = pipeline.upload(outputInit.data(), kOutputWidth, kOutputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    textureGuard.track(output);

    DlssSRProcessor processor(ctx, ngx);

    DlssSRFrameInput frame{};
    frame.color = color.image;
    frame.colorView = color.view;
    frame.depth = depth.image;
    frame.depthView = depth.view;
    frame.motionVectors = motion.image;
    frame.motionView = motion.view;
    frame.diffuseAlbedo = diffuse.image;
    frame.diffuseView = diffuse.view;
    frame.normals = normals.image;
    frame.normalsView = normals.view;
    frame.roughness = roughness.image;
    frame.roughnessView = roughness.view;
    frame.output = output.image;
    frame.outputView = output.view;
    frame.inputWidth = kInputWidth;
    frame.inputHeight = kInputHeight;
    frame.outputWidth = kOutputWidth;
    frame.outputHeight = kOutputHeight;
    frame.jitterX = 0.0f;
    frame.jitterY = 0.0f;
    frame.mvScaleX = 1.0f / static_cast<float>(kInputWidth);
    frame.mvScaleY = 1.0f / static_cast<float>(kInputHeight);
    frame.reset = true;

    VkCommandBuffer evalCmdBuf = allocateCommandBuffer(ctx);
    beginCommandBuffer(evalCmdBuf);
    transitionImage(evalCmdBuf, output.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    REQUIRE(processor.evaluate(evalCmdBuf, frame, errorMsg));
    transitionImage(evalCmdBuf, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submitAndWait(ctx, evalCmdBuf);

    const std::vector<float> result = pipeline.download(output);
    REQUIRE(output.width == kOutputWidth);
    REQUIRE(output.height == kOutputHeight);
    REQUIRE(result.size() == static_cast<size_t>(kOutputWidth) * static_cast<size_t>(kOutputHeight) * 4u);
    REQUIRE(std::any_of(result.begin(), result.end(), [](float value) { return value != 0.0f; }));
}
