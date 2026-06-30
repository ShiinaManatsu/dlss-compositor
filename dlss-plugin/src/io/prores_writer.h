#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dlss {

enum class PixelEncoding;

float inferHdrMasteringNits(std::string_view viewTransform);

enum class VideoCodec {
    ProRes422HQ,
    H265Hevc,
};

struct H265EncoderOptions {
    int cq = 18;
    std::string preset = "p5";
    std::string multipass = "qres";
    bool spatialAq = true;
    bool temporalAq = true;
    bool vfr = false;
};

class ProResWriter {
public:
    ProResWriter(const std::filesystem::path& outputPath,
                 std::uint32_t width,
                 std::uint32_t height,
                 int fps,
                 bool isHdr,
                 PixelEncoding pixelEncoding,
                 float hdrMasteringNits = 1000.0f,
                 int queueHighWatermark = 0,
                 int queueLowWatermark = 0,
                 int encoderThreadCount = 0,
                 VideoCodec codec = VideoCodec::ProRes422HQ,
                 H265EncoderOptions h265Options = {});
    ~ProResWriter();

    ProResWriter(const ProResWriter&) = delete;
    ProResWriter& operator=(const ProResWriter&) = delete;

    void writeFrame(const std::vector<std::uint8_t>& rgba16f);
    void writePackedYuv422Frame(const std::vector<std::uint8_t>& packedWords);
    void enqueueFrame(std::vector<std::uint8_t> rgba16f, std::function<void()> completionCallback = {});
    void enqueuePackedYuv422Frame(std::vector<std::uint8_t> packedWords, std::function<void()> completionCallback = {});
    void cancel();
    void close();

private:
    void encodeFrame(void* frame);
    void writeFrameImpl(const std::vector<std::uint8_t>& rgba16f);
    void writePackedFrameImpl(const std::vector<std::uint8_t>& packedWords);

    struct Impl;
    Impl* m_impl = nullptr;
};

class ProResProgressWriter {
public:
    ProResProgressWriter(const std::filesystem::path& outputPath,
                         std::uint32_t width,
                         std::uint32_t height,
                         int fps,
                         bool isHdr,
                         PixelEncoding pixelEncoding,
                         float hdrMasteringNits,
                         int queueHighWatermark,
                         int queueLowWatermark,
                         int encoderThreadCount,
                         int totalFrames,
                         std::function<void(int current, int total)> progressCallback,
                         VideoCodec codec = VideoCodec::ProRes422HQ,
                         H265EncoderOptions h265Options = {});

    void writeFrame(const std::vector<std::uint8_t>& rgba16f);
    void writePackedYuv422Frame(const std::vector<std::uint8_t>& packedWords);
    void enqueueFrame(std::vector<std::uint8_t> rgba16f);
    void enqueuePackedYuv422Frame(std::vector<std::uint8_t> packedWords);
    void cancel();
    void close();

private:
    ProResWriter m_writer;
    int m_total = 0;
    int m_written = 0;
    std::function<void(int current, int total)> m_progressCallback;
};

} // namespace dlss
