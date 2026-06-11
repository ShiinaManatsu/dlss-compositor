#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace dlss {

class ProResWriter {
public:
    ProResWriter(const std::filesystem::path& outputPath,
                 std::uint32_t width,
                 std::uint32_t height,
                 int fps,
                 bool isHdr);
    ~ProResWriter();

    ProResWriter(const ProResWriter&) = delete;
    ProResWriter& operator=(const ProResWriter&) = delete;

    void writeFrame(const std::vector<std::uint8_t>& rgba16f);
    void close();

private:
    void encodeFrame(void* frame);

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
                         int totalFrames,
                         std::function<void(int current, int total)> progressCallback);

    void writeFrame(const std::vector<std::uint8_t>& rgba16f);
    void close();

private:
    ProResWriter m_writer;
    int m_total = 0;
    int m_written = 0;
    std::function<void(int current, int total)> m_progressCallback;
};

} // namespace dlss
