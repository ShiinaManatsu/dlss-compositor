#pragma once
// VideoReader — FFmpeg demux/decode for the NR tool's color source.
//
// Decodes ProRes/H265 exports (the Stage-A main output) into display-ready
// RGBA8 frames. Sequential decode with backward-seek for scrub access; the
// NR tool scrubs small windows, so this is plenty.
//
// Colorspace: YUV sources are converted limited→full RGB with the source's
// recorded matrix (defaults to BT.709 limited, what our ProRes/H265 writers
// produce). This inverts prores_writer's encode exactly.

#include <cstdint>
#include <filesystem>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace dlss {

class VideoReader {
public:
    VideoReader() = default;
    ~VideoReader();

    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    /// Open `file`; throws std::runtime_error when demux/decode cannot start
    /// or the file has no video stream.
    void open(const std::filesystem::path& file, bool nrGuidance = false);
    void close();

    bool     isOpen() const { return m_fmtCtx != nullptr; }
    uint32_t width() const  { return m_width; }
    uint32_t height() const { return m_height; }
    double   fps() const    { return m_fps; }

    /// Best-effort frame count (nb_frames, else duration × fps). 0 when unknown.
    int64_t frameCount() const { return m_frameCount; }

    /// Decode and convert frame `index` (0-based, CFR assumption) into
    /// `outRgba8` (width*height*4 bytes). Seeks when jumping. Throws on error.
    void readFrameRgba8(int64_t index, std::vector<std::uint8_t>& outRgba8);
    void readNrGuidance(int64_t index, std::vector<std::uint8_t>& mv, std::vector<float>& depth);

private:
    int64_t frameIndexFromPts(int64_t pts) const;
    /// Decode forward until the next frame is returned. Returns false at EOF.
    bool decodeNextFrame();
    void decodeFrameAt(int64_t index);
    void ensureSws(int srcRange, int srcColorspace);

    AVFormatContext* m_fmtCtx    = nullptr;
    AVCodecContext*  m_codecCtx  = nullptr;
    AVFrame*         m_frame     = nullptr;
    AVPacket*        m_packet    = nullptr;
    SwsContext*      m_sws       = nullptr;
    int              m_stream    = -1;
    int64_t          m_frameCount = 0;
    double           m_fps       = 0.0;
    int              m_tbNum     = 0;   // stream time_base (pts → seconds = num/den)
    int              m_tbDen     = 1;
    uint32_t         m_width     = 0;
    uint32_t         m_height    = 0;
    int64_t          m_nextIndex = 0;   // sequential decode position
    bool             m_swsReady  = false;
    bool             m_draining  = false;
};

} // namespace dlss
