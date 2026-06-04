#include <catch2/catch_test_macros.hpp>

#include "../src/cli/config_executor.h"

#include <filesystem>

namespace {

std::filesystem::path tempOutputDir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("CLI config executor reports missing input frames", "[unit][cli]") {
    const auto out = tempOutputDir("dlss_cli_executor_sr");
    dlss::ConfigExecutionOptions options;
    options.configPath = "tests/fixtures/sample-dlssconfig.json";
    options.outputDir = out;
    options.outputTemplate = "frame_####.png";
    options.outputFormat = "png_16";

    REQUIRE(dlss::executeConfig(options) == 4);
    REQUIRE_FALSE(std::filesystem::exists(out / "frame_0001.png"));

    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

TEST_CASE("CLI config executor rejects missing FG inputs", "[unit][cli]") {
    const auto out = tempOutputDir("dlss_cli_executor_fg");
    dlss::ConfigExecutionOptions options;
    options.configPath = "tests/fixtures/sample-dlssconfig.json";
    options.outputDir = out;
    options.outputTemplate = "frame_####.exr";
    options.outputFormat = "exr";
    options.fgEnabled = true;
    options.fgMultiplier = 2;

    REQUIRE(dlss::executeConfig(options) == 4);
    REQUIRE_FALSE(std::filesystem::exists(out / "frame_0001.exr"));

    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}
