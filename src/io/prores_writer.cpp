#include "prores_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <system_error>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}

namespace dlss {
namespace {

std::string fferr(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

float halfToFloat(std::uint16_t bits)
{
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    std::uint32_t exp = (bits >> 10) & 0x1fu;
    std::uint32_t mant = bits & 0x03ffu;

    std::uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03ffu;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        out = sign | 0x7f800000u | (mant << 13);
    } else {
        exp += 127u - 15u;
        out = sign | (exp << 23) | (mant << 13);
    }

    float value = 0.0f;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

std::uint16_t readLe16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

float srgbEncode(float linear) noexcept
{
    if (!std::isfinite(linear)) {
        return 0.0f;
    }
    const float c = std::clamp(linear, 0.0f, 1.0f);
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

int clamp10(float value) noexcept
{
    return std::clamp(static_cast<int>(std::lround(value)), 64, 960);
}

struct YuvPixel {
    int y = 64;
    int u = 512;
    int v = 512;
};

YuvPixel rgbaHalfToBt709Yuv(const std::uint8_t* pixel) noexcept
{
    const float r = srgbEncode(halfToFloat(readLe16(pixel + 0)));
    const float g = srgbEncode(halfToFloat(readLe16(pixel + 2)));
    const float b = srgbEncode(halfToFloat(readLe16(pixel + 4)));
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float cb = (b - y) / 1.8556f;
    const float cr = (r - y) / 1.5748f;
    return YuvPixel{
        clamp10(64.0f + y * 876.0f),
        clamp10(512.0f + cb * 896.0f),
        clamp10(512.0f + cr * 896.0f),
    };
}

} // namespace

struct ProResWriter::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int64_t nextPts = 0;
    bool closed = false;
};

ProResWriter::ProResWriter(const std::filesystem::path& outputPath,
                           std::uint32_t width,
                           std::uint32_t height,
                           int fps,
                           bool isHdr)
{
    if (isHdr) {
        throw std::runtime_error("HDR ProRes export is not supported yet; use EXR scene-linear output");
    }
    if (width == 0 || height == 0 || fps <= 0) {
        throw std::runtime_error("ProResWriter: invalid dimensions or fps");
    }
    m_impl = new Impl{};
    m_impl->width = width;
    m_impl->height = height;
    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    int err = avformat_alloc_output_context2(&m_impl->format, nullptr, "mov", outputPath.string().c_str());
    if (err < 0 || !m_impl->format) {
        throw std::runtime_error("ProResWriter: cannot create MOV muxer: " + fferr(err));
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_PRORES);
    }
    if (!codec) {
        throw std::runtime_error("ProResWriter: FFmpeg ProRes encoder not available");
    }

    m_impl->stream = avformat_new_stream(m_impl->format, nullptr);
    if (!m_impl->stream) {
        throw std::runtime_error("ProResWriter: cannot allocate stream");
    }

    m_impl->codec = avcodec_alloc_context3(codec);
    if (!m_impl->codec) {
        throw std::runtime_error("ProResWriter: cannot allocate codec context");
    }
    m_impl->codec->codec_id = AV_CODEC_ID_PRORES;
    m_impl->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    m_impl->codec->width = static_cast<int>(width);
    m_impl->codec->height = static_cast<int>(height);
    m_impl->codec->time_base = AVRational{1, fps};
    m_impl->codec->framerate = AVRational{fps, 1};
    m_impl->codec->pix_fmt = AV_PIX_FMT_YUV422P10LE;
    m_impl->codec->profile = FF_PROFILE_PRORES_HQ;
    m_impl->codec->color_primaries = AVCOL_PRI_BT709;
    m_impl->codec->color_trc = AVCOL_TRC_BT709;
    m_impl->codec->colorspace = AVCOL_SPC_BT709;
    m_impl->codec->color_range = AVCOL_RANGE_MPEG;
    if (m_impl->format->oformat->flags & AVFMT_GLOBALHEADER) {
        m_impl->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (m_impl->codec->priv_data) {
        av_opt_set_int(m_impl->codec->priv_data, "profile", 3, 0);
    }

    err = avcodec_open2(m_impl->codec, codec, nullptr);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: cannot open encoder: " + fferr(err));
    }
    err = avcodec_parameters_from_context(m_impl->stream->codecpar, m_impl->codec);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: cannot copy codec parameters: " + fferr(err));
    }
    m_impl->stream->time_base = m_impl->codec->time_base;

    if (!(m_impl->format->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&m_impl->format->pb, outputPath.string().c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            throw std::runtime_error("ProResWriter: cannot open output: " + fferr(err));
        }
    }
    err = avformat_write_header(m_impl->format, nullptr);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: cannot write MOV header: " + fferr(err));
    }

    m_impl->frame = av_frame_alloc();
    m_impl->packet = av_packet_alloc();
    if (!m_impl->frame || !m_impl->packet) {
        throw std::runtime_error("ProResWriter: cannot allocate frame/packet");
    }
    m_impl->frame->format = m_impl->codec->pix_fmt;
    m_impl->frame->width = m_impl->codec->width;
    m_impl->frame->height = m_impl->codec->height;
    m_impl->frame->color_primaries = m_impl->codec->color_primaries;
    m_impl->frame->color_trc = m_impl->codec->color_trc;
    m_impl->frame->colorspace = m_impl->codec->colorspace;
    m_impl->frame->color_range = m_impl->codec->color_range;
    err = av_frame_get_buffer(m_impl->frame, 32);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: cannot allocate frame buffer: " + fferr(err));
    }
}

ProResWriter::~ProResWriter()
{
    try {
        close();
    } catch (...) {
    }
}

void ProResWriter::encodeFrame(void* rawFrame)
{
    AVFrame* frame = static_cast<AVFrame*>(rawFrame);
    int err = avcodec_send_frame(m_impl->codec, frame);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: send frame failed: " + fferr(err));
    }
    while (err >= 0) {
        err = avcodec_receive_packet(m_impl->codec, m_impl->packet);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            return;
        }
        if (err < 0) {
            throw std::runtime_error("ProResWriter: receive packet failed: " + fferr(err));
        }
        av_packet_rescale_ts(m_impl->packet, m_impl->codec->time_base, m_impl->stream->time_base);
        m_impl->packet->stream_index = m_impl->stream->index;
        err = av_interleaved_write_frame(m_impl->format, m_impl->packet);
        av_packet_unref(m_impl->packet);
        if (err < 0) {
            throw std::runtime_error("ProResWriter: write packet failed: " + fferr(err));
        }
    }
}

void ProResWriter::writeFrame(const std::vector<std::uint8_t>& rgba16f)
{
    if (!m_impl || m_impl->closed) {
        throw std::runtime_error("ProResWriter: write after close");
    }
    const std::size_t expected = static_cast<std::size_t>(m_impl->width) * m_impl->height * 4u * sizeof(std::uint16_t);
    if (rgba16f.size() < expected) {
        throw std::runtime_error("ProResWriter: frame buffer too small");
    }
    int err = av_frame_make_writable(m_impl->frame);
    if (err < 0) {
        throw std::runtime_error("ProResWriter: frame not writable: " + fferr(err));
    }

    auto* yPlane = reinterpret_cast<std::uint16_t*>(m_impl->frame->data[0]);
    auto* uPlane = reinterpret_cast<std::uint16_t*>(m_impl->frame->data[1]);
    auto* vPlane = reinterpret_cast<std::uint16_t*>(m_impl->frame->data[2]);
    const int yStride = m_impl->frame->linesize[0] / static_cast<int>(sizeof(std::uint16_t));
    const int uStride = m_impl->frame->linesize[1] / static_cast<int>(sizeof(std::uint16_t));
    const int vStride = m_impl->frame->linesize[2] / static_cast<int>(sizeof(std::uint16_t));

    for (std::uint32_t y = 0; y < m_impl->height; ++y) {
        for (std::uint32_t x = 0; x < m_impl->width; x += 2) {
            const std::uint8_t* p0 = rgba16f.data() + (static_cast<std::size_t>(y) * m_impl->width + x) * 8u;
            const std::uint8_t* p1 = (x + 1 < m_impl->width)
                ? p0 + 8u
                : p0;
            const YuvPixel a = rgbaHalfToBt709Yuv(p0);
            const YuvPixel b = rgbaHalfToBt709Yuv(p1);
            yPlane[y * yStride + x] = static_cast<std::uint16_t>(a.y);
            if (x + 1 < m_impl->width) {
                yPlane[y * yStride + x + 1] = static_cast<std::uint16_t>(b.y);
            }
            const std::uint32_t chromaX = x / 2;
            uPlane[y * uStride + chromaX] = static_cast<std::uint16_t>((a.u + b.u + 1) / 2);
            vPlane[y * vStride + chromaX] = static_cast<std::uint16_t>((a.v + b.v + 1) / 2);
        }
    }

    m_impl->frame->pts = m_impl->nextPts++;
    encodeFrame(m_impl->frame);
}

void ProResWriter::close()
{
    if (!m_impl || m_impl->closed) {
        return;
    }
    encodeFrame(nullptr);
    const int err = av_write_trailer(m_impl->format);
    m_impl->closed = true;
    if (!(m_impl->format->oformat->flags & AVFMT_NOFILE) && m_impl->format->pb) {
        avio_closep(&m_impl->format->pb);
    }
    av_packet_free(&m_impl->packet);
    av_frame_free(&m_impl->frame);
    avcodec_free_context(&m_impl->codec);
    avformat_free_context(m_impl->format);
    delete m_impl;
    m_impl = nullptr;
    if (err < 0) {
        throw std::runtime_error("ProResWriter: write trailer failed: " + fferr(err));
    }
}

ProResProgressWriter::ProResProgressWriter(const std::filesystem::path& outputPath,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           int fps,
                                           bool isHdr,
                                           int totalFrames,
                                           std::function<void(int current, int total)> progressCallback)
    : m_writer(outputPath, width, height, fps, isHdr),
      m_total(totalFrames),
      m_progressCallback(std::move(progressCallback))
{
}

void ProResProgressWriter::writeFrame(const std::vector<std::uint8_t>& rgba16f)
{
    m_writer.writeFrame(rgba16f);
    ++m_written;
    if (m_progressCallback) {
        m_progressCallback(m_written, m_total);
    }
}

void ProResProgressWriter::close()
{
    m_writer.close();
    if (m_progressCallback) {
        m_progressCallback(m_total, m_total);
    }
}

} // namespace dlss
