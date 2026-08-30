#include <fstream>
#include <iterator>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#ifndef DLSS_SOURCE_DIR
#define DLSS_SOURCE_DIR "."
#endif

namespace {

std::string readTextFile(const char* path)
{
    std::ifstream in(std::filesystem::path(DLSS_SOURCE_DIR) / path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("ProResWriter configures FFmpeg threading", "[io][prores]")
{
    const std::string source = readTextFile("cli/src/io/prores_writer.cpp");
    CHECK(source.find("thread_count = m_impl->encoderThreadCount") != std::string::npos);
    CHECK(source.find("thread_type = FF_THREAD_SLICE") != std::string::npos);
}

TEST_CASE("Video writer configures HEVC MP4 path", "[io][video]")
{
    const std::string source = readTextFile("cli/src/io/prores_writer.cpp");
    CHECK(source.find("VideoCodec::H265Hevc") != std::string::npos);
    CHECK(source.find("AV_CODEC_ID_HEVC") != std::string::npos);
    CHECK(source.find("AV_PIX_FMT_P010LE") != std::string::npos);
    CHECK(source.find("\"mp4\"") != std::string::npos);
}

TEST_CASE("Video template extension handling knows MP4", "[cli][video]")
{
    const std::string source = readTextFile("cli/src/cli/config_executor.cpp");
    CHECK(source.find("currentExt == \".mp4\"") != std::string::npos);
}

TEST_CASE("SR-only ProRes path uses queued writes", "[pipeline][prores]")
{
    const std::string source = readTextFile("cli/src/pipeline/sr_only_pipeline.cpp");
    CHECK(source.find("proresWriter->enqueuePackedYuv422Frame") != std::string::npos);
    CHECK(source.find("proresConvertPass->convertAndReadback") != std::string::npos);
}

TEST_CASE("ProresConvertPass reuses descriptor set across frames", "[gpu][prores]")
{
    const std::string source = readTextFile("cli/src/gpu/passes/prores_convert_pass.cpp");
    const auto recordPos = source.find("void ProresConvertPass::record");
    REQUIRE(recordPos != std::string::npos);
    const std::string recordBody = source.substr(recordPos);

    CHECK(source.find("vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_descSet)") != std::string::npos);
    CHECK(recordBody.find("vkAllocateDescriptorSets") == std::string::npos);
    CHECK(recordBody.find("&m_descSet") != std::string::npos);
}
