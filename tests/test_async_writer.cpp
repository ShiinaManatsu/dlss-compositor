// =============================================================================
// AsyncFrameWriter / EncoderRegistry unit tests (T29).
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "../src/io/async_writer.h"
#include "../src/io/encoder_registry.h"
#include "../src/io/prores_writer.h"

#include <filesystem>
#include <fstream>
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

    // v2 implements ONLY these image-sequence formats.
    REQUIRE(reg.hasEncoder(dlss::OutputFormat::ExrLinear));
    REQUIRE(reg.hasEncoder(dlss::OutputFormat::Png16));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::ProRes422HQ)); // sequence-level ProResWriter, not per-frame registry

    // Extension placeholders must NOT be registered.
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Jpeg));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::H264Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::H265Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Av1Mp4));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::Avif));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::ProRes4444XQ));
    REQUIRE_FALSE(reg.hasEncoder(dlss::OutputFormat::DNxHR));
}

TEST_CASE("AsyncFrameWriter - writes real PNG16", "[unit]")
{
    const auto output = std::filesystem::temp_directory_path() / "test_async_writer_png16.png";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    dlss::AsyncFrameWriter writer(1);
    dlss::WriteJob job;
    job.format = dlss::OutputFormat::Png16;
    job.is_hdr = false;
    job.width = 1;
    job.height = 1;
    job.outputPath = output;
    job.pixels = {
        0x00, 0x3c, // R = 1.0 half
        0x00, 0x00, // G = 0.0 half
        0x00, 0x00, // B = 0.0 half
        0x00, 0x3c, // A = 1.0 half
    };

    REQUIRE_NOTHROW(writer.enqueue(job));
    REQUIRE_NOTHROW(writer.flush());
    REQUIRE_FALSE(writer.hasErrors());

    std::ifstream in(output, std::ios::binary);
    REQUIRE(in.good());
    std::string header(33, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    REQUIRE(static_cast<std::size_t>(in.gcount()) == header.size());
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(header.data());
    REQUIRE(bytes[0] == 0x89);
    REQUIRE(bytes[1] == 'P');
    REQUIRE(bytes[2] == 'N');
    REQUIRE(bytes[3] == 'G');
    REQUIRE(bytes[24] == 16); // IHDR bit depth
    REQUIRE(bytes[25] == 6);  // IHDR RGBA color type

    std::filesystem::remove(output, ec);
}

TEST_CASE("ProResWriter - writes HDR ProRes 422 HQ", "[unit][prores]")
{
    const auto output = std::filesystem::temp_directory_path() / "test_prores_hdr_422hq.mov";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    std::vector<std::uint8_t> frame;
    frame.reserve(2 * 2 * 4 * 2);
    auto appendHalf = [&](std::uint16_t value) {
        frame.push_back(static_cast<std::uint8_t>(value & 0xff));
        frame.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    };
    for (int i = 0; i < 4; ++i) {
        appendHalf(0x3800); // R = 0.5, already display-ready for HDR/PQ path.
        appendHalf(0x3400); // G = 0.25
        appendHalf(0x3000); // B = 0.125
        appendHalf(0x3c00); // A = 1.0
    }

    REQUIRE_NOTHROW([&] {
        dlss::ProResWriter writer(output, 2, 2, 24, true, dlss::PixelEncoding::DisplayReady);
        writer.writeFrame(frame);
        writer.close();
    }());

    REQUIRE(std::filesystem::exists(output));
    REQUIRE(std::filesystem::file_size(output) > 0);

    std::ifstream mov(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(mov)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.find("colr") != std::string::npos);
    REQUIRE(bytes.find("mdcv") != std::string::npos);
    REQUIRE(bytes.find("clli") != std::string::npos);

    std::filesystem::remove(output, ec);
}

TEST_CASE("ProResWriter - infers HDR mastering nits from Blender view transform", "[unit][prores]")
{
    REQUIRE(dlss::inferHdrMasteringNits("ACES 2.0 - HDR 1000 nits") == 1000.0f);
    REQUIRE(dlss::inferHdrMasteringNits("ACES 2.0 - HDR 500 nits") == 500.0f);
    REQUIRE(dlss::inferHdrMasteringNits("ACES 2.0") == 1000.0f);
}

TEST_CASE("ProResProgressWriter - reports final frame as complete", "[unit][prores]")
{
    const auto output = std::filesystem::temp_directory_path() / "test_prores_progress.mov";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    std::vector<std::uint8_t> frame;
    frame.reserve(2 * 2 * 4 * 2);
    auto appendHalf = [&](std::uint16_t value) {
        frame.push_back(static_cast<std::uint8_t>(value & 0xff));
        frame.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    };
    for (int i = 0; i < 4; ++i) {
        appendHalf(0x3800);
        appendHalf(0x3800);
        appendHalf(0x3800);
        appendHalf(0x3c00);
    }

    int reportedCurrent = -1;
    int reportedTotal = -1;
    dlss::ProResProgressWriter writer(
        output,
        2,
        2,
        24,
        false,
        dlss::PixelEncoding::DisplayReady,
        1000.0f,
        1,
        [&](int current, int total) {
            reportedCurrent = current;
            reportedTotal = total;
        });
    writer.writeFrame(frame);
    REQUIRE(reportedCurrent == 1);
    REQUIRE(reportedTotal == 1);
    writer.close();

    std::filesystem::remove(output, ec);
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
