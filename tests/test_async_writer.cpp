// =============================================================================
// AsyncFrameWriter / EncoderRegistry unit tests (T29).
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "../src/io/async_writer.h"
#include "../src/io/encoder_registry.h"

#include <filesystem>
#include <stdexcept>
#include <string>

TEST_CASE("AsyncFrameWriter - format path template ####", "[unit]")
{
    using dlss::AsyncFrameWriter;
    REQUIRE(AsyncFrameWriter::formatPath("frame_####.exr", 42) == "frame_0042.exr");
    REQUIRE(AsyncFrameWriter::formatPath("frame_####.exr", 1)  == "frame_0001.exr");
    REQUIRE(AsyncFrameWriter::formatPath("####.png",       100) == "0100.png");
}

TEST_CASE("AsyncFrameWriter - format path {frame:04d}", "[unit]")
{
    using dlss::AsyncFrameWriter;
    REQUIRE(AsyncFrameWriter::formatPath("{frame:04d}.exr", 7)  == "0007.exr");
    REQUIRE(AsyncFrameWriter::formatPath("{frame:04d}.png", 42) == "0042.png");
}

TEST_CASE("AsyncFrameWriter - HDR + PNG rejected", "[unit]")
{
    dlss::AsyncFrameWriter writer(1);
    dlss::WriteJob job;
    job.format     = dlss::OutputFormat::Png16;
    job.is_hdr     = true;
    job.width      = 64;
    job.height     = 64;
    job.pixels.resize(static_cast<std::size_t>(64) * 64 * 8, 0);
    job.outputPath = std::filesystem::temp_directory_path() / "test_hdr.png";

    REQUIRE_THROWS_AS(writer.enqueue(job), std::runtime_error);
}

TEST_CASE("EncoderRegistry - only v2 formats registered", "[unit]")
{
    auto& reg = dlss::globalEncoderRegistry();

    // v2 implements ONLY these 3.
    REQUIRE(reg.hasEncoder(dlss::OutputFormat::ExrLinear));
    REQUIRE(reg.hasEncoder(dlss::OutputFormat::Png16));
    REQUIRE(reg.hasEncoder(dlss::OutputFormat::ProRes422HQ));

    // Extension placeholders must NOT be registered.
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Jpeg));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::H264Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::H265Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Av1Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Avif));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::ProRes4444XQ));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::DNxHR));
}

TEST_CASE("AsyncFrameWriter - flush waits for all jobs", "[unit]")
{
    dlss::AsyncFrameWriter writer(2);

    // Enqueue some LDR EXR jobs (should process without error).
    std::vector<std::filesystem::path> outputs;
    for (int i = 0; i < 5; ++i) {
        dlss::WriteJob job;
        job.format = dlss::OutputFormat::ExrLinear;
        job.is_hdr = false;
        job.width  = 4;
        job.height = 4;
        job.pixels.resize(static_cast<std::size_t>(4) * 4 * 8, 0);
        job.outputPath = std::filesystem::temp_directory_path() /
                         ("test_async_writer_frame_" + std::to_string(i) + ".exr");
        outputs.push_back(job.outputPath);
        REQUIRE_NOTHROW(writer.enqueue(job));
    }

    REQUIRE_NOTHROW(writer.flush());
    REQUIRE_FALSE(writer.hasErrors());

    // Cleanup
    std::error_code ec;
    for (const auto& p : outputs) {
        std::filesystem::remove(p, ec);
    }
}
