#include "prores_writer.h"

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "encoder_registry.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mastering_display_metadata.h>
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

AVRational rat(int num, int den) noexcept
{
    return AVRational{num, den};
}

void addHdr10SideData(AVStream* stream, float masteringNits)
{
    if (!stream || !stream->codecpar) {
        throw std::runtime_error("ProResWriter: stream missing for HDR metadata");
    }

    const float clampedNits = std::clamp(masteringNits, 100.0f, 10000.0f);
    auto* masteringSideData = av_packet_side_data_new(
        &stream->codecpar->coded_side_data,
        &stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
        sizeof(AVMasteringDisplayMetadata),
        0);
    if (!masteringSideData) {
        throw std::runtime_error("ProResWriter: cannot allocate HDR mastering metadata");
    }

    auto* mastering = reinterpret_cast<AVMasteringDisplayMetadata*>(masteringSideData->data);
    std::memset(mastering, 0, sizeof(*mastering));
    mastering->display_primaries[0][0] = rat(708, 1000);
    mastering->display_primaries[0][1] = rat(292, 1000);
    mastering->display_primaries[1][0] = rat(170, 1000);
    mastering->display_primaries[1][1] = rat(797, 1000);
    mastering->display_primaries[2][0] = rat(131, 1000);
    mastering->display_primaries[2][1] = rat(46, 1000);
    mastering->white_point[0] = rat(3127, 10000);
    mastering->white_point[1] = rat(3290, 10000);
    mastering->min_luminance = rat(1, 10000);
    mastering->max_luminance = rat(static_cast<int>(std::lround(clampedNits)), 1);
    mastering->has_primaries = 1;
    mastering->has_luminance = 1;

    auto* lightSideData = av_packet_side_data_new(
        &stream->codecpar->coded_side_data,
        &stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
        sizeof(AVContentLightMetadata),
        0);
    if (!lightSideData) {
        throw std::runtime_error("ProResWriter: cannot allocate HDR content light metadata");
    }

    auto* light = reinterpret_cast<AVContentLightMetadata*>(lightSideData->data);
    std::memset(light, 0, sizeof(*light));
    light->MaxCLL = static_cast<unsigned>(std::lround(clampedNits));
    light->MaxFALL = static_cast<unsigned>(std::lround(clampedNits * 0.4f));
}

struct YuvPixel {
    int y = 64;
    int u = 512;
    int v = 512;
};

YuvPixel rgbaHalfToYuv(const std::uint8_t* pixel, PixelEncoding pixelEncoding, bool isHdr) noexcept
{
    // ProRes stores non-linear Y'CbCr. Scene-linear LDR output needs an
    // sRGB/709-like OETF here; LUT/FG output is already display-ready and must
    // not be gamma-encoded a second time. HDR ProRes expects display-ready
    // Rec.2100/PQ code values from the Blender LUT path.
    const bool needsSrgbEncode = !isHdr && pixelEncoding == PixelEncoding::SceneLinear;
    const auto encode = [needsSrgbEncode](float value) noexcept {
        return needsSrgbEncode ? srgbEncode(value) : std::clamp(value, 0.0f, 1.0f);
    };
    const float rp = encode(halfToFloat(readLe16(pixel + 0)));
    const float gp = encode(halfToFloat(readLe16(pixel + 2)));
    const float bp = encode(halfToFloat(readLe16(pixel + 4)));
    const float kr = isHdr ? 0.2627f : 0.2126f;
    const float kb = isHdr ? 0.0593f : 0.0722f;
    const float kg = 1.0f - kr - kb;
    const float yp = kr * rp + kg * gp + kb * bp;
    const float cb = (bp - yp) / (2.0f * (1.0f - kb));
    const float cr = (rp - yp) / (2.0f * (1.0f - kr));
    return YuvPixel{
        clamp10(64.0f + yp * 876.0f),
        clamp10(512.0f + cb * 896.0f),
        clamp10(512.0f + cr * 896.0f),
    };
}

} // namespace

float inferHdrMasteringNits(std::string_view viewTransform)
{
    const std::string text(viewTransform);
    const auto nitsPos = text.find("nits");
    const auto nitPos = nitsPos == std::string::npos ? text.find("nit") : nitsPos;
    if (nitPos == std::string::npos) {
        return 1000.0f;
    }

    std::size_t end = nitPos;
    while (end > 0 && text[end - 1] == ' ') {
        --end;
    }
    std::size_t begin = end;
    while (begin > 0) {
        const char c = text[begin - 1];
        if ((c >= '0' && c <= '9') || c == '.') {
            --begin;
            continue;
        }
        break;
    }
    if (begin == end) {
        return 1000.0f;
    }
    try {
        return std::clamp(std::stof(text.substr(begin, end - begin)), 100.0f, 10000.0f);
    } catch (...) {
        return 1000.0f;
    }
}

struct ProResWriter::Impl {
    struct QueuedFrame {
        std::vector<std::uint8_t> pixels;
        std::function<void()> completionCallback;
    };

    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int64_t nextPts = 0;
    bool closed = false;
    bool isHdr = false;
    float hdrMasteringNits = 1000.0f;
    PixelEncoding pixelEncoding = PixelEncoding::SceneLinear;

    std::mutex encodeMutex;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<QueuedFrame> pendingFrames;
    std::thread worker;
    bool stopWorker = false;
    bool cancelRequested = false;
    std::exception_ptr workerError;
};

ProResWriter::ProResWriter(const std::filesystem::path& outputPath,
                           std::uint32_t width,
                           std::uint32_t height,
                           int fps,
                           bool isHdr,
                           PixelEncoding pixelEncoding,
                           float hdrMasteringNits)
{
    if (width == 0 || height == 0 || fps <= 0) {
        throw std::runtime_error("ProResWriter: invalid dimensions or fps");
    }
    m_impl = new Impl{};
    m_impl->width = width;
    m_impl->height = height;
    m_impl->isHdr = isHdr;
    m_impl->hdrMasteringNits = std::clamp(hdrMasteringNits, 100.0f, 10000.0f);
    m_impl->pixelEncoding = pixelEncoding;
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
    m_impl->codec->color_primaries = isHdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    m_impl->codec->color_trc = isHdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
    m_impl->codec->colorspace = isHdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
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
    if (isHdr) {
        addHdr10SideData(m_impl->stream, m_impl->hdrMasteringNits);
    }
    m_impl->stream->time_base = m_impl->codec->time_base;

    if (!(m_impl->format->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&m_impl->format->pb, outputPath.string().c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            throw std::runtime_error("ProResWriter: cannot open output: " + fferr(err));
        }
    }
    AVDictionary* muxOptions = nullptr;
    if (isHdr) {
        av_dict_set(&muxOptions, "movflags", "+write_colr", 0);
    }
    err = avformat_write_header(m_impl->format, &muxOptions);
    av_dict_free(&muxOptions);
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

    m_impl->worker = std::thread([this]() {
        for (;;) {
            Impl::QueuedFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_impl->queueMutex);
                m_impl->queueCv.wait(lock, [this]() {
                    return m_impl->stopWorker || !m_impl->pendingFrames.empty();
                });
                if (m_impl->cancelRequested) {
                    m_impl->pendingFrames.clear();
                }
                if (m_impl->pendingFrames.empty()) {
                    if (m_impl->stopWorker) {
                        return;
                    }
                    continue;
                }
                frame = std::move(m_impl->pendingFrames.front());
                m_impl->pendingFrames.pop_front();
            }

            try {
                writeFrameImpl(frame.pixels);
                if (frame.completionCallback) {
                    frame.completionCallback();
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(m_impl->queueMutex);
                if (!m_impl->workerError) {
                    m_impl->workerError = std::current_exception();
                }
                m_impl->pendingFrames.clear();
                return;
            }
        }
    });
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
    if (!m_impl) {
        throw std::runtime_error("ProResWriter: encoder is closed");
    }
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
    writeFrameImpl(rgba16f);
}

void ProResWriter::enqueueFrame(std::vector<std::uint8_t> rgba16f, std::function<void()> completionCallback)
{
    if (!m_impl || m_impl->closed) {
        throw std::runtime_error("ProResWriter: enqueue after close");
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        if (m_impl->workerError) {
            std::rethrow_exception(m_impl->workerError);
        }
        m_impl->pendingFrames.push_back(Impl::QueuedFrame{
            std::move(rgba16f),
            std::move(completionCallback),
        });
    }
    m_impl->queueCv.notify_one();
}

void ProResWriter::cancel()
{
    if (!m_impl || m_impl->closed) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        m_impl->cancelRequested = true;
        m_impl->stopWorker = true;
        m_impl->pendingFrames.clear();
    }
    m_impl->queueCv.notify_one();
    close();
}

void ProResWriter::writeFrameImpl(const std::vector<std::uint8_t>& rgba16f)
{
    if (!m_impl || m_impl->closed) {
        throw std::runtime_error("ProResWriter: write after close");
    }
    const std::size_t expected = static_cast<std::size_t>(m_impl->width) * m_impl->height * 4u * sizeof(std::uint16_t);
    if (rgba16f.size() < expected) {
        throw std::runtime_error("ProResWriter: frame buffer too small");
    }
    std::lock_guard<std::mutex> encodeLock(m_impl->encodeMutex);

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
            const YuvPixel a = rgbaHalfToYuv(p0, m_impl->pixelEncoding, m_impl->isHdr);
            const YuvPixel b = rgbaHalfToYuv(p1, m_impl->pixelEncoding, m_impl->isHdr);
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

    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        m_impl->stopWorker = true;
    }
    m_impl->queueCv.notify_one();
    if (m_impl->worker.joinable()) {
        m_impl->worker.join();
    }

    std::exception_ptr workerError;
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        workerError = m_impl->workerError;
    }

    int err = 0;
    std::exception_ptr closeError;
    try {
        encodeFrame(nullptr);
        err = av_write_trailer(m_impl->format);
    } catch (...) {
        closeError = std::current_exception();
    }
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
    if (workerError) {
        std::rethrow_exception(workerError);
    }
    if (closeError) {
        std::rethrow_exception(closeError);
    }
    if (err < 0) {
        throw std::runtime_error("ProResWriter: write trailer failed: " + fferr(err));
    }
}

ProResProgressWriter::ProResProgressWriter(const std::filesystem::path& outputPath,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           int fps,
                                           bool isHdr,
                                           PixelEncoding pixelEncoding,
                                           float hdrMasteringNits,
                                           int totalFrames,
                                           std::function<void(int current, int total)> progressCallback)
    : m_writer(outputPath, width, height, fps, isHdr, pixelEncoding, hdrMasteringNits),
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

void ProResProgressWriter::enqueueFrame(std::vector<std::uint8_t> rgba16f)
{
    m_writer.enqueueFrame(std::move(rgba16f), [this]() {
        ++m_written;
        if (m_progressCallback) {
            m_progressCallback(m_written, m_total);
        }
    });
}

void ProResProgressWriter::cancel()
{
    m_writer.cancel();
}

void ProResProgressWriter::close()
{
    m_writer.close();
    if (m_progressCallback) {
        m_progressCallback(m_total, m_total);
    }
}

} // namespace dlss
