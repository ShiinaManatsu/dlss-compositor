#include <catch2/catch_test_macros.hpp>

#include "../src/cli/config.h"
#include "core/exr_reader.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "pipeline/sequence_processor.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

size_t countExrFiles(const std::filesystem::path& directory) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".exr") {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("e2e_sequence_processing", "[integration]") {
    std::filesystem::remove_all("test_integration_out");
    std::filesystem::create_directories("test_integration_out");

    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No RTX GPU");
    }

    NgxContext ngx;
    if (!ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg)) {
        SKIP("NGX init failed: " + errorMsg);
    }
    if (!ngx.isDlssRRAvailable()) {
        SKIP("DLSS-RR not available");
    }

    {
        TexturePipeline pipeline(ctx);
        SequenceProcessor processor(ctx, ngx, pipeline);

        AppConfig config;
        config.inputDir = "tests/fixtures/sequence/";
        config.outputDir = "test_integration_out/";
        config.scaleFactor = 2;

        const bool result = processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg);
        INFO(errorMsg);
        REQUIRE(result == true);

        REQUIRE(std::filesystem::exists(config.outputDir));
        REQUIRE(countExrFiles(config.outputDir) == 5);

        const std::filesystem::path firstOutput = std::filesystem::path(config.outputDir) / "frame_0001.exr";
        REQUIRE(std::filesystem::exists(firstOutput));

        ExrReader reader;
        REQUIRE(reader.open(firstOutput.string(), errorMsg));
        REQUIRE(reader.width() == 128);
        REQUIRE(reader.height() == 128);
    }

    ngx.shutdown();
    ctx.destroy();
    std::filesystem::remove_all("test_integration_out");
}

TEST_CASE("dlss_fg_e2e_interpolation", "[integration][fg]") {
    struct OutputDirCleanup {
        ~OutputDirCleanup() {
            std::filesystem::remove_all("test_fg_integration_out/");
        }
    } cleanup;

    std::filesystem::remove_all("test_fg_integration_out/");
    std::filesystem::create_directories("test_fg_integration_out/");

    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No RTX GPU");
    }

    NgxContext ngx;
    if (!ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg)) {
        SKIP("NGX init failed: " + errorMsg);
    }
    if (!ngx.isDlssFGAvailable()) {
        SKIP("DLSS-G not available");
    }

    {
        TexturePipeline pipeline(ctx);
        SequenceProcessor processor(ctx, ngx, pipeline);

        AppConfig config;
        config.inputDir = "tests/fixtures/sequence/";
        config.outputDir = "test_fg_integration_out/";
        config.scaleFactor = 1;
        config.interpolateFactor = 2;
        config.cameraDataFile = "tests/fixtures/camera.json";

        std::filesystem::create_directories(config.outputDir);

        const bool result = processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg);
        INFO(errorMsg);
        REQUIRE(result == true);

        REQUIRE(std::filesystem::exists(config.outputDir));
        // 5 input frames × 2x interpolation: 5 originals + 4 interpolated = 9 outputs
        REQUIRE(countExrFiles(config.outputDir) == 9);

        const std::filesystem::path firstOutput = std::filesystem::path(config.outputDir) / "frame_0001.exr";
        REQUIRE(std::filesystem::exists(firstOutput));

        ExrReader reader;
        REQUIRE(reader.open(firstOutput.string(), errorMsg));
        REQUIRE(reader.width() == 64);
        REQUIRE(reader.height() == 64);

        const std::filesystem::path interpOutput = std::filesystem::path(config.outputDir) / "frame_0002.exr";
        REQUIRE(std::filesystem::exists(interpOutput));

        ExrReader interpReader;
        REQUIRE(interpReader.open(interpOutput.string(), errorMsg));
        REQUIRE(interpReader.width() == 64);
        REQUIRE(interpReader.height() == 64);

        const float* interpPixels = interpReader.readChannel("R");
        REQUIRE(interpPixels != nullptr);

        float maxPixel = 0.0f;
        const int pixelCount = interpReader.width() * interpReader.height();
        for (int i = 0; i < pixelCount; ++i) {
            maxPixel = std::max(maxPixel, interpPixels[i]);
        }
        REQUIRE(maxPixel > 1.0f);
    }

    ngx.shutdown();
    ctx.destroy();
    std::filesystem::remove_all("test_fg_integration_out/");
}

TEST_CASE("dlss_rrfg_e2e_combined", "[integration][rrfg]") {
    struct OutputDirCleanup {
        ~OutputDirCleanup() {
            std::filesystem::remove_all("test_rrfg_integration_out/");
        }
    } cleanup;

    std::filesystem::remove_all("test_rrfg_integration_out/");
    std::filesystem::create_directories("test_rrfg_integration_out/");

    VulkanContext ctx;
    std::string errorMsg;
    if (!ctx.init(errorMsg)) {
        SKIP("No RTX GPU");
    }

    NgxContext ngx;
    if (!ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg)) {
        SKIP("NGX init failed: " + errorMsg);
    }
    if (!ngx.isDlssRRAvailable()) {
        SKIP("DLSS-RR not available");
    }
    if (!ngx.isDlssFGAvailable()) {
        SKIP("DLSS-G not available");
    }

    {
        TexturePipeline pipeline(ctx);
        SequenceProcessor processor(ctx, ngx, pipeline);

        AppConfig config;
        config.inputDir = "tests/fixtures/sequence/";
        config.outputDir = "test_rrfg_integration_out/";
        config.scaleFactor = 2;
        config.interpolateFactor = 2;
        config.cameraDataFile = "tests/fixtures/camera.json";

        std::filesystem::create_directories(config.outputDir);

        const bool result = processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg);
        INFO(errorMsg);
        REQUIRE(result == true);

        REQUIRE(std::filesystem::exists(config.outputDir));
        // 5 input frames × 2x interpolation: 5 originals + 4 interpolated = 9 outputs
        REQUIRE(countExrFiles(config.outputDir) == 9);

        const std::filesystem::path firstOutput = std::filesystem::path(config.outputDir) / "frame_0001.exr";
        REQUIRE(std::filesystem::exists(firstOutput));

        ExrReader reader;
        REQUIRE(reader.open(firstOutput.string(), errorMsg));
        REQUIRE(reader.width() == 128);
        REQUIRE(reader.height() == 128);

        const std::filesystem::path interpOutput = std::filesystem::path(config.outputDir) / "frame_0002.exr";
        REQUIRE(std::filesystem::exists(interpOutput));

        ExrReader interpReader;
        REQUIRE(interpReader.open(interpOutput.string(), errorMsg));
        REQUIRE(interpReader.width() == 128);
        REQUIRE(interpReader.height() == 128);

        const float* interpPixels = interpReader.readChannel("R");
        REQUIRE(interpPixels != nullptr);

        float maxPixel = 0.0f;
        const int pixelCount = interpReader.width() * interpReader.height();
        for (int i = 0; i < pixelCount; ++i) {
            maxPixel = std::max(maxPixel, interpPixels[i]);
        }
        REQUIRE(maxPixel > 1.0f);
    }

    ngx.shutdown();
    ctx.destroy();
    std::filesystem::remove_all("test_rrfg_integration_out/");
}
