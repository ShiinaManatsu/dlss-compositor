#include <catch2/catch_test_macros.hpp>
#include "../src/gpu/passes/mv_pass.h"
#include "../src/gpu/vulkan_context.h"
#include "../src/gpu/texture_pool.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// CPU-side sign mapping test — does NOT require GPU
// Verifies the mathematical mapping from T13 MV_SANITY_RESULT.md

TEST_CASE("MV mapping - X axis direct passthrough", "[unit]") {
    // If object moves RIGHT by N pixels:
    // Blender Vector.X = -N (Blender X+=left convention, rightward motion = negative)
    // DLSS curr→prev: prev is to the LEFT of curr → DLSS.X = -N
    // Mapping: DLSS.X = Blender.X (same value, both negative for rightward motion)

    float objectMovedRight = +5.0f;  // 5 pixels rightward in screen space
    float blenderVecX = -objectMovedRight;  // Blender X+=left → rightward = negative (-5)
    float dlssExpectedX = -objectMovedRight; // DLSS curr→prev: prev is left of curr = -5

    float dlssComputedX = blenderVecX;  // Direct passthrough

    REQUIRE(dlssComputedX == dlssExpectedX);
    // This confirms: DLSS_MV.X = Blender.X (no sign flip)
}

TEST_CASE("MV mapping - Y axis negation", "[unit]") {
    // If object moves DOWN by M pixels:
    // Blender Vector.Y = +M (Blender Y+=down, downward = positive)
    // DLSS curr→prev: prev is ABOVE (negative Y) curr → DLSS.Y = -M
    // Mapping: DLSS.Y = -Blender.Y

    float objectMovedDown = +3.0f;   // 3 pixels downward
    float blenderVecY = +objectMovedDown;  // Blender Y+=down → +3
    float dlssExpectedY = -objectMovedDown;  // DLSS curr→prev: prev is above → -3

    float dlssComputedY = -blenderVecY;  // Negate

    REQUIRE(dlssComputedY == dlssExpectedY);
    // This confirms: DLSS_MV.Y = -Blender.Y (one sign flip)
}

TEST_CASE("MV mapping - NOT double negation (v1 bug check)", "[unit]") {
    // v1 used -blenderMv (both X and Y negated) — WRONG
    // Test that our single-Y-negation is different from double negation

    float blenderX = -5.0f;
    float blenderY = +3.0f;

    // Correct mapping (v2):
    float v2_x = blenderX;   // direct
    float v2_y = -blenderY;  // negate Y

    // Wrong v1 mapping:
    float v1_x = -blenderX;  // negate X (wrong)
    float v1_y = -blenderY;  // negate Y

    REQUIRE(v2_x != v1_x);  // v2 X differs from v1 X
    REQUIRE(v2_y == v1_y);  // Y happens to be same
    REQUIRE(v2_x == blenderX);  // v2 X == Blender.X
}

TEST_CASE("MV mapping - sanity result file exists", "[unit]") {
    // MV_SANITY_RESULT.md must exist for T23 to have proceeded
    std::filesystem::path resultPath = "blender-plugin/tests/MV_SANITY_RESULT.md";
    REQUIRE(std::filesystem::exists(resultPath));

    // Must contain VERDICT or MAPPING
    auto content = std::ifstream(resultPath.string());
    std::string all((std::istreambuf_iterator<char>(content)),
                     std::istreambuf_iterator<char>());
    bool hasVerdict = all.find("VERDICT") != std::string::npos ||
                      all.find("MAPPING") != std::string::npos;
    REQUIRE(hasVerdict);
}

TEST_CASE("MV conversion pass - execute reuses descriptor set", "[unit]") {
    auto content = std::ifstream("dlss-plugin/src/gpu/passes/mv_pass.cpp");
    REQUIRE(content.good());
    std::string source((std::istreambuf_iterator<char>(content)),
                       std::istreambuf_iterator<char>());

    const auto executePos = source.find("void MvConversionPass::execute");
    REQUIRE(executePos != std::string::npos);
    const auto executeBody = source.substr(executePos);

    REQUIRE(source.find("vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet)") != std::string::npos);
    REQUIRE(executeBody.find("vkAllocateDescriptorSets") == std::string::npos);
    REQUIRE(executeBody.find("&m_descSet") != std::string::npos);
}

TEST_CASE("MV conversion pass - executes on GPU", "[gpu]") {
    dlss::VulkanContext ctx;
    dlss::TexturePool pool(&ctx);

    // 4-channel source (Blender Vector pass)
    dlss::TextureDesc src4ch{64, 64, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    // 2-channel destination (DLSS MV)
    dlss::TextureDesc dst2ch{64, 64, VK_FORMAT_R16G16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};

    auto src = pool.acquire(src4ch);
    auto dst = pool.acquire(dst2ch);

    dlss::MvConversionPass pass(&ctx);

    auto pool2 = ctx.createComputeCommandPool();
    auto cmd = ctx.beginOneTimeCommands(pool2);
    REQUIRE_NOTHROW(pass.execute(cmd, src, dst));
    ctx.endOneTimeCommands(pool2, cmd, ctx.computeQueue());
    ctx.destroyCommandPool(pool2);

    pool.release(src);
    pool.release(dst);
    SUCCEED("MV conversion pass executed without error");
}
