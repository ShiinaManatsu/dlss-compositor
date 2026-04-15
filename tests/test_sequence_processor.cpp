#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "core/exr_reader.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "pipeline/sequence_processor.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

TEST_CASE("sequence_processor_5_frames", "[gpu][sequence]") {
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

    TexturePipeline pipeline(ctx);
    SequenceProcessor processor(ctx, ngx, pipeline);

    AppConfig config;
    config.inputDir = "tests/fixtures/sequence";
    config.outputDir = "test_out";
    config.scaleFactor = 2;

    std::filesystem::remove_all(config.outputDir);
    REQUIRE(processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg));

    REQUIRE(std::filesystem::exists(config.outputDir));
    REQUIRE(countExrFiles(config.outputDir) == countExrFiles(config.inputDir));

    const std::filesystem::path firstOutput = std::filesystem::path(config.outputDir) / "frame_0001.exr";
    REQUIRE(std::filesystem::exists(firstOutput));
    REQUIRE(std::filesystem::file_size(firstOutput) > 0);

    ExrReader reader;
    REQUIRE(reader.open(firstOutput.string(), errorMsg));
    REQUIRE(reader.width() == 128);
    REQUIRE(reader.height() == 128);

    std::filesystem::remove_all(config.outputDir);
    ngx.shutdown();
    ctx.destroy();
}

TEST_CASE("sequence_processor_gap_detection", "[sequence]") {
    const std::filesystem::path tempDir = std::filesystem::path("test_out") / "sequence_gap_detection";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    const std::vector<std::string> filenames = {
        "frame_0001.exr",
        "frame_0002.exr",
        "frame_0004.exr",
        "frame_0010.exr",
    };
    for (const std::string& filename : filenames) {
        std::ofstream file(tempDir / filename);
        file << '\n';
    }

    std::vector<SequenceFrameInfo> frames;
    std::string errorMsg;
    REQUIRE(SequenceProcessor::scanAndSort(tempDir, frames, errorMsg));
    REQUIRE(frames.size() == filenames.size());
    REQUIRE(frames[0].path.filename().string() == "frame_0001.exr");
    REQUIRE(frames[1].path.filename().string() == "frame_0002.exr");
    REQUIRE(frames[2].path.filename().string() == "frame_0004.exr");
    REQUIRE(frames[3].path.filename().string() == "frame_0010.exr");

    const std::vector<bool> resets = SequenceProcessor::computeResetFlags(frames);
    REQUIRE(resets.size() == frames.size());
    REQUIRE(resets[0]);
    REQUIRE(!resets[1]);
    REQUIRE(resets[2]);
    REQUIRE(resets[3]);

    std::filesystem::remove_all(tempDir);
}
