// =============================================================================
// SrOnlyPipeline tests (T36).
//
// Verifies the Stage 1 → 2 → 7 orchestration contract and regression guards:
//   - SR-only path must not call Stage 4/5/6.
//   - InReset covers first frame, prefetcher gap marker, and camera-cut hook.
//   - Frame time comes from fps.
//   - The pipeline loop acquires from TexturePool instead of creating images.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "../src/pipeline/sr_only_pipeline.h"
#include "../src/dlss/ngx_wrapper.h"
#include "../src/gpu/vulkan_context.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        m_dir = fs::temp_directory_path() /
                ("dlss_sr_only_" + std::to_string(rd()));
        fs::create_directories(m_dir);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return m_dir; }

private:
    fs::path m_dir;
};

void writeFixtureExr(const fs::path& filePath, int width, int height, float value) {
    Imf::Header header(width, height);
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Z", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Vector.X", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Vector.Y", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Vector.Z", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Vector.W", Imf::Channel(Imf::FLOAT));

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> rgba(pixels * 4, value);
    std::vector<float> z(pixels, 1.0f);
    std::vector<float> vec(pixels * 4, 0.0f);

    Imf::FrameBuffer fb;
    const std::size_t pixRgba = 4 * sizeof(float);
    const std::size_t rowRgba = pixRgba * static_cast<std::size_t>(width);
    const std::size_t pixZ    = sizeof(float);
    const std::size_t rowZ    = pixZ * static_cast<std::size_t>(width);

    const char* const rgbaNames[4] = {"R", "G", "B", "A"};
    for (int ch = 0; ch < 4; ++ch) {
        fb.insert(rgbaNames[ch], Imf::Slice(Imf::FLOAT,
            reinterpret_cast<char*>(rgba.data() + ch), pixRgba, rowRgba));
    }
    fb.insert("Z", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(z.data()), pixZ, rowZ));

    const char* const vecNames[4] = {"Vector.X", "Vector.Y", "Vector.Z", "Vector.W"};
    for (int ch = 0; ch < 4; ++ch) {
        fb.insert(vecNames[ch], Imf::Slice(Imf::FLOAT,
            reinterpret_cast<char*>(vec.data() + ch), pixRgba, rowRgba));
    }

    Imf::OutputFile out(filePath.string().c_str(), header);
    out.setFrameBuffer(fb);
    out.writePixels(height);
}

void identity(float (&m)[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

dlss::CameraData cameraForFrame(int frame) {
    dlss::CameraData cam{};
    identity(cam.matrix_world);
    identity(cam.projection);
    cam.matrix_world[3][0] = static_cast<float>(frame) * 0.01f;
    cam.fov = 1.0471976f;
    cam.aspect_ratio = 1.0f;
    cam.near_clip = 0.1f;
    cam.far_clip = 100.0f;
    cam.jitter = {0.0f, 0.0f};
    return cam;
}

dlss::SrOnlyConfig makeConfig(const fs::path& inputDir, const fs::path& outputDir) {
    dlss::SrOnlyConfig cfg;
    cfg.outputDir = outputDir.string();
    cfg.filenameTemplate = "sr_####.exr";
    cfg.outputFormat = dlss::OutputFormat::ExrLinear;
    cfg.overwriteMode = dlss::OverwriteMode::Overwrite;
    cfg.dlssConfig.output_resolution = {64, 64};
    cfg.dlssConfig.render_resolution = {32, 32};
    cfg.dlssConfig.quality = dlss::QualityMode::Performance;
    cfg.dlssConfig.is_hdr = false;
    cfg.dlssConfig.fps = 24;
    cfg.dlssConfig.filename_pattern = (inputDir / "frame_####.exr").string();

    for (int i = 1; i <= 5; ++i) {
        char key[5] = {};
        std::snprintf(key, sizeof(key), "%04d", i);
        cfg.dlssConfig.frames.emplace(key, cameraForFrame(i));
    }
    return cfg;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("SrOnlyPipeline - static contract keeps SR path free of FG and inverse", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_only_pipeline.cpp");
    REQUIRE(source.find("DlssFgPass") == std::string::npos);
    REQUIRE(source.find("InverseTonemapPass") == std::string::npos);
    REQUIRE(source.find("LutTonemapPass") != std::string::npos);
    REQUIRE(source.find("config.outputFormat == OutputFormat::ExrLinear") != std::string::npos);
    REQUIRE(source.find("1000.0f / static_cast<float>(dlssCfg.fps)") != std::string::npos);
    REQUIRE(source.find("frame.has_gap_after") != std::string::npos);
    REQUIRE(source.find("cameraCutThreshold = INFINITY") != std::string::npos);
}

TEST_CASE("SrOnlyPipeline - pipeline loop uses TexturePool acquire instead of vkCreateImage", "[unit][pipeline]") {
    const std::string source = readTextFile("src/pipeline/sr_only_pipeline.cpp");
    const auto loopPos = source.find("for (int frameIdx = 0; frameIdx < totalFrames");
    REQUIRE(loopPos != std::string::npos);
    const auto loopBody = source.substr(loopPos);
    REQUIRE(loopBody.find("pool.acquire") != std::string::npos);
    REQUIRE(loopBody.find("pool.release") != std::string::npos);
    REQUIRE(loopBody.find("vkCreateImage") == std::string::npos);
}

TEST_CASE("SrOnlyPipeline - constructor rejects missing runtime dependencies", "[unit][pipeline]") {
    dlss::RpcDispatcher dispatcher;
    REQUIRE_THROWS_AS(dlss::SrOnlyPipeline(nullptr, nullptr, &dispatcher), std::invalid_argument);
}

TEST_CASE("SrOnlyPipeline - runs five-frame SR path", "[gpu][pipeline]") {
    TempDir tmp;
    const fs::path inputDir = tmp.path() / "input";
    const fs::path outputDir = tmp.path() / "output";
    fs::create_directories(inputDir);
    fs::create_directories(outputDir);

    for (int i = 1; i <= 5; ++i) {
        char name[32] = {};
        std::snprintf(name, sizeof(name), "frame_%04d.exr", i);
        writeFixtureExr(inputDir / name, 32, 32, 0.1f * static_cast<float>(i));
    }

    dlss::VulkanContext ctx;
    dlss::NgxContext ngx(ctx.instance(), ctx.physicalDevice(), ctx.device());
    REQUIRE(ngx.isSrAvailable());

    dlss::RpcDispatcher dispatcher;
    dlss::SrOnlyPipeline pipeline(&ctx, &ngx, &dispatcher);
    REQUIRE_NOTHROW(pipeline.run(makeConfig(inputDir, outputDir)));

    REQUIRE(dispatcher.progress().current_frame.load() == 5);
    REQUIRE(dispatcher.progress().total_frames.load() == 5);
    REQUIRE(dispatcher.progress().state.load() == 2);
}
