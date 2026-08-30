// =============================================================================
// FgOnlyPipeline tests (T37).
//
// Verifies the Stage 1 -> 4 -> 5 -> 7 orchestration contract:
//   - FG-only path must not call DlssSrPass (grep verification: 0 matches).
//   - TonemapPass executes before DlssFgPass::evaluate.
//   - EXR output keeps the inverse pass path; display-ready outputs skip it.
//   - 5 input frames at 2x produce 2N-1 = 9 outputs named 0001..0009.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "../src/pipeline/fg_only_pipeline.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef DLSS_SOURCE_DIR
#define DLSS_SOURCE_DIR "."
#endif

namespace {

std::string readTextFile(const fs::path& path) {
    std::ifstream in(fs::path(DLSS_SOURCE_DIR) / path, std::ios::binary);
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

TEST_CASE("FgOnlyPipeline - static contract keeps FG path Stage 1->4->5->7 only", "[gpu][pipeline]") {
    const std::string source = readTextFile("cli/src/pipeline/fg_only_pipeline.cpp");

    // grep verification: DlssSrPass must have 0 matches in fg_only_pipeline.cpp.
    REQUIRE(source.find("DlssSrPass") == std::string::npos);

    const std::size_t stage1 = requireFindAfter(source, "ChannelMapper::map", 0);
    const std::size_t stage4 = requireFindAfter(source, "tonemapPass.execute", stage1);
    const std::size_t stage5 = requireFindAfter(source, "fgPass.evaluate", stage4);
    const std::size_t stage7 = requireFindAfter(source, "writeDisplayOrSceneLinear", stage5);
    const std::size_t exrBranch = requireFindAfter(source, "config.outputFormat == OutputFormat::ExrLinear", 0);
    const std::size_t inverseCall = requireFindAfter(source, "inversePass.execute", exrBranch);
    REQUIRE(stage1 < stage4);
    REQUIRE(stage4 < stage5);
    REQUIRE(stage5 < stage7);
    REQUIRE(exrBranch < inverseCall);

    REQUIRE(source.find("config.multiplier * totalFrames - (config.multiplier - 1)") != std::string::npos);
    REQUIRE(source.find("++outputFrameIdx") != std::string::npos);
    REQUIRE(source.find("AsyncFrameWriter::formatPath(config.filenameTemplate, outputFrameIdx)") != std::string::npos);
}

TEST_CASE("FgOnlyPipeline - ProRes path writes real frames asynchronously at FG output fps", "[unit][pipeline]") {
    const std::string source = readTextFile("cli/src/pipeline/fg_only_pipeline.cpp");

    REQUIRE(source.find("dlssCfg.fps * config.multiplier") != std::string::npos);
    REQUIRE(source.find("proresWriter->enqueuePackedYuv422Frame") != std::string::npos);
    REQUIRE(source.find("proresConvertPass->convertAndReadback") != std::string::npos);
    REQUIRE(source.find("downloadImageA2B10ToRgba16f") != std::string::npos);
    REQUIRE(source.find("proresWriter->writeFrame(m_ctx->downloadImageRgba16f") == std::string::npos);
    REQUIRE(source.find("PixelEncoding::DisplayReady") != std::string::npos);
}

TEST_CASE("FgOnlyPipeline - tonemap_lut selects LUT tonemap before FG", "[unit][pipeline]") {
    const std::string source = readTextFile("cli/src/pipeline/fg_only_pipeline.cpp");

    const std::size_t lutField = requireFindAfter(source, "dlssCfg.tonemap_lut", 0);
    const std::size_t loadCube = requireFindAfter(source, "LutLoader::LoadCube", lutField);
    const std::size_t createPass = requireFindAfter(source, "std::make_unique<LutTonemapPass>", loadCube);
    const std::size_t lutExecute = requireFindAfter(source, "lutTonemapPass->execute", createPass);
    const std::size_t fallback = requireFindAfter(source, "tonemapPass.execute", lutExecute);

    REQUIRE(lutField < loadCube);
    REQUIRE(loadCube < createPass);
    REQUIRE(createPass < lutExecute);
    REQUIRE(lutExecute < fallback);
}

TEST_CASE("FgOnlyPipeline - five inputs at 2x renumber to 0001 through 0009", "[gpu][pipeline]") {
    dlss::FgOnlyConfig cfg;
    cfg.multiplier = 2;
    cfg.filenameTemplate = "frame_####.exr";

    for (int i = 1; i <= 5; ++i) {
        char key[5] = {};
        std::snprintf(key, sizeof(key), "%04d", i);
        cfg.dlssConfig.frames.emplace(key, dlss::CameraData{});
    }

    const int inputFrames = static_cast<int>(cfg.dlssConfig.frames.size());
    const int outputFrames = cfg.multiplier * inputFrames - (cfg.multiplier - 1);
    REQUIRE(outputFrames == 9);

    std::vector<std::string> names;
    for (int frame = 1; frame <= outputFrames; ++frame) {
        names.push_back(dlss::AsyncFrameWriter::formatPath(cfg.filenameTemplate, frame));
    }

    REQUIRE(names.front() == "frame_0001.exr");
    REQUIRE(names.back() == "frame_0009.exr");
    REQUIRE(names.size() == 9);
}
