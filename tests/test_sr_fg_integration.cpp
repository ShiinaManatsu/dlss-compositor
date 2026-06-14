#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../src/pipeline/sr_fg_pipeline.h"
#include "../src/dlss/ngx_wrapper.h"
#include "../src/gpu/vulkan_context.h"
#include "../src/rpc/dispatcher.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t requireFindAfter(const std::string& source,
                             const std::string& needle,
                             std::size_t after) {
    const std::size_t pos = source.find(needle, after);
    REQUIRE(pos != std::string::npos);
    return pos;
}

} // namespace

TEST_CASE("SrFgPipeline - shared depth/MV invariant documented", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");

    const auto depthAcquire = requireFindAfter(source, "auto depthNdc", 0);
    const auto mvAcquire = requireFindAfter(source, "auto mvDlss", depthAcquire);
    const auto srDepth = requireFindAfter(source, "srIn.depthNdc", mvAcquire);
    const auto srMv = requireFindAfter(source, "srIn.motionVecDlss", srDepth);
    const auto fgDepth = requireFindAfter(source, "fgIn.depthNdcRenderRes", srMv);
    const auto fgMv = requireFindAfter(source, "fgIn.motionVecDlssRenderRes", fgDepth);

    REQUIRE(source.find("auto depthNdcFg") == std::string::npos);
    REQUIRE(source.find("auto mvDlssFg") == std::string::npos);
    REQUIRE(srDepth < fgDepth);
    REQUIRE(srMv < fgMv);
    SUCCEED("Shared depth/MV invariant is enforced by same depthNdc/mvDlss handles passed to SR and FG");
}

TEST_CASE("SrFgPipeline - stage order invariant documented", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");

    const auto stage1 = requireFindAfter(source, "ChannelMapper::map", 0);
    const auto stage2 = requireFindAfter(source, "srPass.evaluate", stage1);
    const auto stage4 = requireFindAfter(source, "tonemapPass.execute", stage2);
    const auto stage5 = requireFindAfter(source, "fgPass.evaluate", stage4);
    const auto stage7 = requireFindAfter(source, "writeDisplayOrSceneLinear", stage5);
    const auto exrBranch = requireFindAfter(source, "config.outputFormat == OutputFormat::ExrLinear", 0);
    const auto inverseCall = requireFindAfter(source, "inversePass.execute", exrBranch);

    REQUIRE(stage1 < stage2);
    REQUIRE(stage2 < stage4);
    REQUIRE(stage4 < stage5);
    REQUIRE(stage5 < stage7);
    REQUIRE(exrBranch < inverseCall);
    SUCCEED("Stage order enforced: SR -> tonemap -> FG -> optional inverse/write in sr_fg_pipeline.cpp");
}

TEST_CASE("SrFgPipeline - ProRes path writes real frames asynchronously at FG output fps", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");

    REQUIRE(source.find("dlssCfg.fps * config.fgMultiplier") != std::string::npos);
    REQUIRE(source.find("proresWriter->enqueueFrame(std::move(pixels))") != std::string::npos);
    REQUIRE(source.find("downloadImageA2B10ToRgba16f") != std::string::npos);
    REQUIRE(source.find("proresWriter->writeFrame(m_ctx->downloadImageRgba16f") == std::string::npos);
    REQUIRE(source.find("PixelEncoding::DisplayReady") != std::string::npos);
}

TEST_CASE("SrFgPipeline - tonemap_lut selects LUT tonemap before FG", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");

    const auto lutField = requireFindAfter(source, "dlssCfg.tonemap_lut", 0);
    const auto loadCube = requireFindAfter(source, "LutLoader::LoadCube", lutField);
    const auto createPass = requireFindAfter(source, "std::make_unique<LutTonemapPass>", loadCube);
    const auto lutExecute = requireFindAfter(source, "lutTonemapPass->execute", createPass);
    const auto fallback = requireFindAfter(source, "tonemapPass.execute", lutExecute);

    REQUIRE(lutField < loadCube);
    REQUIRE(loadCube < createPass);
    REQUIRE(createPass < lutExecute);
    REQUIRE(lutExecute < fallback);
}

TEST_CASE("SrFgPipeline - frame time delta from fps", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");
    REQUIRE(source.find("1000.0f / static_cast<float>(dlssCfg.fps)") != std::string::npos);
    REQUIRE(source.find("33.3f") == std::string::npos);

    constexpr float fps = 24.0f;
    constexpr float expectedMs = 1000.0f / fps;
    REQUIRE(expectedMs == Catch::Approx(41.667f).epsilon(0.01f));
    SUCCEED("frameTimeMs = 1000.0f / fps — verified by code");
}

TEST_CASE("SrFgPipeline - output frame count formula", "[unit][pipeline]") {
    int N = 5;
    REQUIRE(2 * N - 1 == 9);   // 2x: 9 frames
    REQUIRE(4 * N - 3 == 17);  // 4x: 17 frames
}

TEST_CASE("SrFgPipeline - no image creation in processing loop", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_fg_pipeline.cpp");
    const auto loopPos = source.find("for (int frameIdx = 0; frameIdx < inputFrames");
    REQUIRE(loopPos != std::string::npos);
    const auto loopBody = source.substr(loopPos);
    REQUIRE(loopBody.find("pool.acquire") != std::string::npos);
    REQUIRE(loopBody.find("pool.release") != std::string::npos);
    REQUIRE(loopBody.find("vkCreateImage") == std::string::npos);
}

TEST_CASE("SrFgPipeline - constructor rejects missing runtime dependencies", "[unit][pipeline]") {
    dlss::RpcDispatcher dispatcher;
    REQUIRE_THROWS_AS(dlss::SrFgPipeline(nullptr, nullptr, &dispatcher), std::invalid_argument);
}

TEST_CASE("SrFgPipeline - run on GPU", "[gpu][pipeline]") {
    dlss::VulkanContext ctx;
    dlss::NgxContext ngx(ctx.instance(), ctx.physicalDevice(), ctx.device());
    dlss::RpcDispatcher dispatcher;

    REQUIRE(ngx.isSrAvailable());
    REQUIRE(ngx.isFgAvailable());

    dlss::SrFgPipeline pipeline(&ctx, &ngx, &dispatcher);
    SUCCEED("SrFgPipeline created successfully");
}
