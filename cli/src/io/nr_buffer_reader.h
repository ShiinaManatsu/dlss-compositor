#pragma once
#include "video_reader.h"
//
// Stage-A guide video (gray16 panels: MV.x / MV.y / depth) or legacy RGB half EXR.
// Read back as RG16F motion bits and R32F depth for NR and FG.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dlss {

class NrBufferReader {
public:
    /// Prefer dlssnrbuffer.mkv; fall back to a legacy EXR sequence.
    void open(const std::filesystem::path& dir);

    bool     isOpen() const { return m_video.isOpen() || !m_files.empty(); }
    size_t   frames() const { return m_video.isOpen() ? static_cast<size_t>(m_video.frameCount()) : m_files.size(); }
    double   fps() const { return m_video.isOpen() ? m_video.fps() : 0.0; }
    uint32_t width() const  { return m_w; }
    uint32_t height() const { return m_h; }

    /// Read frame `idx`: `mvOut` = w*h RG16F bit pairs (w*h*4 bytes),
    /// `depthOut` = w*h NDC depth floats. Throws on IO/decoding errors.
    void readFrame(std::size_t idx,
                   std::vector<std::uint8_t>& mvOut,
                   std::vector<float>&        depthOut);

private:
    VideoReader m_video;
    std::vector<std::filesystem::path> m_files;
    uint32_t m_w = 0;
    uint32_t m_h = 0;
    // EXR channel indices resolved by NAME at open(). EXR stores the
    // ChannelList alphabetically (B,G,R on disk); OIIO presents write order,
    // so indices would work too — name lookup is immune to writer variance.
    int chMvX = 0;
    int chMvY = 0;
    int chDepth = 0;
};

} // namespace dlss
