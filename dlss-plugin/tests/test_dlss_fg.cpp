#include <catch2/catch_test_macros.hpp>

#include "../src/dlss/dlss_fg.h"

#include <volk.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

void identity(float (&m)[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

dlss::FgInputs makeFgInputs(
    VkImageView backbuffer,
    VkImageView depth,
    VkImageView motionVectors,
    VkImageView output,
    int multiplier) {
    dlss::FgInputs inputs{};
    inputs.backbufferDisplayReady = backbuffer;
    inputs.depthNdcRenderRes = depth;
    inputs.motionVecDlssRenderRes = motionVectors;
    inputs.outputInterp = {output};
    inputs.width = 1280;
    inputs.height = 720;
    identity(inputs.viewToClip);
    identity(inputs.clipToPrevClip);
    identity(inputs.prevClipToClip);
    inputs.camNear = 0.1f;
    inputs.camFar = 1000.0f;
    inputs.fov = 1.0471976f;
    inputs.aspectRatio = 16.0f / 9.0f;
    inputs.camUpY = 1.0f;
    inputs.camRightX = 1.0f;
    inputs.camFwdZ = -1.0f;
    inputs.is_hdr = true;
    inputs.reset = true;
    inputs.multiplier = multiplier;
    return inputs;
}

} // namespace

TEST_CASE("DlssFgPass - normalizes pixel motion vectors for FG", "[unit]") {
    std::ifstream in("dlss-plugin/src/dlss/dlss_fg.cpp", std::ios::binary);
    REQUIRE(in.good());
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE(source.find("1.0f / static_cast<float>(depthMotionWidth)") != std::string::npos);
    REQUIRE(source.find("1.0f / static_cast<float>(depthMotionHeight)") != std::string::npos);
    REQUIRE(source.find("mvecsSubrectSize = {depthMotionWidth, depthMotionHeight}") != std::string::npos);
    REQUIRE(source.find("backbufferSubrectSize = {in.width, in.height}") != std::string::npos);
}

TEST_CASE("DlssFgPass - create/destroy FG feature", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);
    dlss::NgxContext ngx(ctx.instance(), ctx.physicalDevice(), ctx.device());
    REQUIRE(ngx.isFgAvailable());

    dlss::DlssFgPass fg(&ngx, &ctx, &pool);
    REQUIRE_FALSE(fg.isCreated());

    REQUIRE_NOTHROW(fg.create(1280, 720, 1280, 720, 2));
    REQUIRE(fg.isCreated());

    fg.destroy();
    REQUIRE_FALSE(fg.isCreated());
}

TEST_CASE("DlssFgPass - 2x evaluate generates one interpolated frame", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);
    dlss::NgxContext ngx(ctx.instance(), ctx.physicalDevice(), ctx.device());
    REQUIRE(ngx.isFgAvailable());

    dlss::DlssFgPass fg(&ngx, &ctx, &pool);
    REQUIRE_NOTHROW(fg.create(1280, 720, 1280, 720, 2));

    const VkImageUsageFlags commonUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto backbuffer = pool.acquire({1280, 720, VK_FORMAT_A2B10G10R10_UNORM_PACK32, commonUsage});
    auto depth = pool.acquire({1280, 720, VK_FORMAT_R32_SFLOAT, commonUsage});
    auto mvec = pool.acquire({1280, 720, VK_FORMAT_R16G16_SFLOAT, commonUsage});
    auto interp = pool.acquire({1280, 720, VK_FORMAT_A2B10G10R10_UNORM_PACK32, commonUsage});

    auto inputs = makeFgInputs(backbuffer->view, depth->view, mvec->view, interp->view, 2);
    inputs.backbufferImage = backbuffer->image;
    inputs.depthImage = depth->image;
    inputs.motionVecImage = mvec->image;
    inputs.outputInterpImages = {interp->image};

    auto cmdPool = ctx.createComputeCommandPool();
    VkCommandBuffer cmd = ctx.beginOneTimeCommands(cmdPool);
    REQUIRE_NOTHROW(fg.evaluate(cmd, inputs));
    ctx.endOneTimeCommands(cmdPool, cmd, ctx.computeQueue());
    ctx.destroyCommandPool(cmdPool);

    pool.release(backbuffer);
    pool.release(depth);
    pool.release(mvec);
    pool.release(interp);
}

TEST_CASE("DlssFgPass - validate multiplier and output count", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);
    dlss::NgxContext ngx(ctx.instance(), ctx.physicalDevice(), ctx.device());
    REQUIRE(ngx.isFgAvailable());

    dlss::DlssFgPass fg(&ngx, &ctx, &pool);
    REQUIRE_THROWS(fg.create(1280, 720, 1280, 720, 1));
    REQUIRE_NOTHROW(fg.create(1280, 720, 1280, 720, 2));

    dlss::FgInputs inputs{};
    inputs.multiplier = 2;
    REQUIRE_THROWS(fg.evaluate(VK_NULL_HANDLE, inputs));
}
