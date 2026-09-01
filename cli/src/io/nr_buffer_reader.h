#pragma once
// NrBufferReader — dlssnrbuffer_####.exr sequence reader for the NR tool.
//
// The buffer is the Stage-A guidance export (RGB half EXR: R=mv.x, G=mv.y,
// B=NDC depth — see encodeExrNrBuffer). This reads it back split into the
// textures DLSSNR consumes: RG16F motion bits + R32F depth.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dlss {

class NrBufferReader {
public:
    /// Enumerate dlssnrbuffer_*.exr in `dir` (sorted by frame number) and take
    /// the dimensions from frame 0. Throws when the sequence is missing/empty.
    void open(const std::filesystem::path& dir);

    bool     isOpen() const { return !m_files.empty(); }
    size_t   frames() const { return m_files.size(); }
    uint32_t width() const  { return m_w; }
    uint32_t height() const { return m_h; }

    /// Read frame `idx`: `mvOut` = w*h RG16F bit pairs (w*h*4 bytes),
    /// `depthOut` = w*h NDC depth floats. Throws on IO/decoding errors.
    void readFrame(std::size_t idx,
                   std::vector<std::uint8_t>& mvOut,
                   std::vector<float>&        depthOut);

private:
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
