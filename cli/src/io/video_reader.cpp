// VideoReader implementation — see video_reader.h.
// FFmpeg is linked statically via vcpkg (same libs prores_writer uses).

#include "video_reader.h"

#include "../core/log.h"

// vcpkg's FFmpeg 8 public headers drop the extern "C" guard — wrap manually
// (same pattern as prores_writer.cpp).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <stdexcept>
#include <string>

namespace dlss {

namespace {
[[noreturn]] void fail(const std::string& what, int avErr = 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (avErr != 0) {
        av_strerror(avErr, buf, sizeof(buf));
        throw std::runtime_error(what + ": " + buf);
    }
    throw std::runtime_error(what);
}
} // namespace

VideoReader::~VideoReader() {
    close();
}

void VideoReader::open(const std::filesystem::path& file) {
    close();

    if (avformat_open_input(&m_fmtCtx, file.string().c_str(), nullptr, nullptr) < 0) {
        fail("VideoReader: cannot open " + file.string());
    }
    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        fail("VideoReader: cannot read stream info: " + file.string());
    }

    const AVCodec* codec = nullptr;
    m_stream = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (m_stream < 0 || codec == nullptr) {
        fail("VideoReader: no video stream: " + file.string());
    }
    AVStream* st = m_fmtCtx->streams[m_stream];

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) fail("VideoReader: cannot allocate codec context");
    if (avcodec_parameters_to_context(m_codecCtx, st->codecpar) < 0) {
        fail("VideoReader: cannot fill codec context");
    }
    m_codecCtx->thread_count = 4;
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        fail("VideoReader: avcodec_open2 failed");
    }

    m_width  = static_cast<uint32_t>(m_codecCtx->width);
    m_height = static_cast<uint32_t>(m_codecCtx->height);
    if (m_width == 0 || m_height == 0) {
        fail("VideoReader: zero dimensions: " + file.string());
    }

    // CFR assumption. Two demuxer-derived rates, both can lie:
    //   avg_frame_rate is DERIVED (frames / stream duration) and comes out
    //   wrong when the muxer stored packets without durations (stream
    //   duration then ends at the last PTS — measured 45/1 on a 30/1 3-frame
    //   ProRes, 2026-08-31).
    //   r_frame_rate is the codec timing — but the mov demuxer echoes the
    //   track tick rate through it when stts carries no real sample
    //   durations (our fragmented ProRes MOVs: 1-tick stts → r = 1/tb =
    //   15360 while avg is the honest 30; spotted via "landed on 512").
    // When r echoes 1/time_base it carries no timing information: trust avg.
    // Otherwise keep r only when the two agree within 1%; on disagreement r
    // is the honest CFR rate for normally-muxed files.
    {
        const AVRational avg = st->avg_frame_rate;
        const AVRational r   = st->r_frame_rate;
        const AVRational tb  = st->time_base;
        const double avgFps  = avg.num > 0 ? static_cast<double>(avg.num) / avg.den : 0.0;
        const double rFps    = r.num > 0 ? static_cast<double>(r.num) / r.den : 0.0;
        const double tbFps   = tb.num > 0 ? static_cast<double>(tb.den) / tb.num : 0.0;
        const bool rEchoesTb = rFps > 0.0 && tbFps > 0.0 &&
                               std::fabs(rFps - tbFps) <= 0.01 * tbFps;
        const bool agree = avgFps > 0.0 && rFps > 0.0 &&
                           std::fabs(avgFps - rFps) <= 0.01 * rFps;
        const AVRational fr = rEchoesTb
            ? avg
            : (agree ? avg : (r.num > 0 ? r : avg));
        m_fps = fr.num > 0 ? static_cast<double>(fr.num) / static_cast<double>(fr.den) : 0.0;
    }
    if (m_fps <= 0.0) fail("VideoReader: unknown frame rate: " + file.string());
    m_tbNum = st->time_base.num;
    m_tbDen = st->time_base.den;

    if (st->nb_frames > 0) {
        m_frameCount = static_cast<int64_t>(st->nb_frames);
    } else if (m_fmtCtx->duration > 0) {
        // duration is in AV_TIME_BASE (microseconds) units.
        m_frameCount = static_cast<int64_t>(std::llround(
            static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE * m_fps));
    }

    m_frame  = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) fail("VideoReader: cannot allocate AVFrame/AVPacket");
    m_nextIndex = 0;

    LOG_INFO("[VideoReader] {}: {}x{} @ {:.3f} fps, ~{} frames (pix_fmt={})",
             file.filename().string(), m_width, m_height, m_fps,
             m_frameCount, av_get_pix_fmt_name(m_codecCtx->pix_fmt));
}

void VideoReader::close() {
    if (m_sws)      { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_packet)   { av_packet_free(&m_packet); }
    if (m_frame)    { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
    if (m_fmtCtx)   { avformat_close_input(&m_fmtCtx); }
    m_stream = -1;
    m_frameCount = 0;
    m_nextIndex = 0;
    m_swsReady = false;
}

int64_t VideoReader::frameIndexFromPts(int64_t pts) const {
    const double seconds = static_cast<double>(pts) *
        static_cast<double>(m_tbNum) / static_cast<double>(m_tbDen);
    return static_cast<int64_t>(std::llround(seconds * m_fps));
}

bool VideoReader::decodeNextFrame() {
    while (true) {
        // Feed one packet (or enter drain mode at EOF), then drain all frames.
        const int send = av_read_frame(m_fmtCtx, m_packet);
        if (send == AVERROR_EOF) {
            const int rc = avcodec_send_packet(m_codecCtx, nullptr);
            if (rc < 0 && rc != AVERROR_EOF) {
                fail("VideoReader: avcodec_send_packet(flush)", rc);
            }
        } else if (send < 0) {
            fail("VideoReader: av_read_frame", send);
        } else if (m_packet->stream_index != m_stream) {
            av_packet_unref(m_packet);
            continue;
        } else {
            const int rc = avcodec_send_packet(m_codecCtx, m_packet);
            av_packet_unref(m_packet);
            if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
                fail("VideoReader: avcodec_send_packet", rc);
            }
        }

        while (true) {
            const int recv = avcodec_receive_frame(m_codecCtx, m_frame);
            if (recv == 0) {
                return true;
            }
            if (recv == AVERROR_EOF) {
                return false;  // drain exhausted
            }
            if (recv == AVERROR(EAGAIN)) {
                break;  // need more input
            }
            fail("VideoReader: avcodec_receive_frame", recv);
        }
    }
}

void VideoReader::ensureSws(int srcRange, int srcColorspace) {
    if (m_sws && m_swsReady) return;

    // ProRes/H265 from our writers: YUV limited-range. Decode metadata when
    // present, else assume BT.709 limited — the exact inverse of the encode.
    // Installed FFmpeg names the BT.709 coefficient set SWS_CS_ITU709.
    const int coefficients = srcColorspace >= 0 ? srcColorspace : SWS_CS_ITU709;

    m_sws = sws_getContext(m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
                           m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_RGBA,
                           SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!m_sws) fail("VideoReader: sws_getContext failed");

    // src limited (0) unless flagged full; dst RGBA8 is full range (1).
    // Brightness/contrast/saturation are 16.16 fixed point — 0 for brightness
    // is neutral, but contrast/saturation neutral is 1<<16 (0 would zero the
    // image: every frame comes out black).
    if (sws_setColorspaceDetails(m_sws,
                                 sws_getCoefficients(coefficients), srcRange,
                                 sws_getCoefficients(SWS_CS_ITU709), 1,
                                 0, 1 << 16, 1 << 16) < 0) {
        fail("VideoReader: sws_setColorspaceDetails failed");
    }
    m_swsReady = true;
}

void VideoReader::readFrameRgba8(int64_t index, std::vector<std::uint8_t>& outRgba8) {
    if (!isOpen()) fail("VideoReader: not open");
    if (index < 0) fail("VideoReader: negative frame index");

    // Jump needed? Seek to the nearest keyframe at/before the target, then
    // decode forward to the exact index.
    if (index != m_nextIndex) {
        const int64_t targetTs = static_cast<int64_t>(
            static_cast<double>(index) / m_fps *
            static_cast<double>(m_tbDen) / static_cast<double>(m_tbNum));
        if (av_seek_frame(m_fmtCtx, m_stream, targetTs,
                          AVSEEK_FLAG_BACKWARD) < 0) {
            avformat_seek_file(m_fmtCtx, m_stream, INT64_MIN, targetTs, targetTs, 0);
        }
        avcodec_flush_buffers(m_codecCtx);
        m_nextIndex = -1;  // recount after seek
    }

    if (m_nextIndex < 0 || index >= m_nextIndex) {
        while (true) {
            if (!decodeNextFrame()) {
                fail("VideoReader: EOF before frame " + std::to_string(index));
            }
            const int64_t idx = frameIndexFromPts(
                m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? m_frame->best_effort_timestamp : m_frame->pts);
            if (m_nextIndex < 0) {
                m_nextIndex = idx;  // first frame after seek anchors the count
            }
            if (idx == index) break;
            if (idx > index) {
                fail("VideoReader: skipped past frame " + std::to_string(index) +
                     " (landed on " + std::to_string(idx) + ") — non-CFR source?");
            }
        }
    }

    ensureSws(m_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
              m_frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709
              : m_frame->colorspace == AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601
              : SWS_CS_ITU709);

    outRgba8.resize(static_cast<std::size_t>(m_width) * m_height * 4);
    uint8_t* dst[4] = {outRgba8.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {static_cast<int>(m_width) * 4, 0, 0, 0};
    if (sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_codecCtx->height,
                  dst, dstStride) < 0) {
        fail("VideoReader: sws_scale failed on frame " + std::to_string(index));
    }

    m_nextIndex = index + 1;
}

} // namespace dlss
