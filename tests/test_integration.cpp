#include <catch2/catch_test_macros.hpp>

#include "../src/cli/config.h"
#include "core/exr_reader.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "pipeline/sequence_processor.h"

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
