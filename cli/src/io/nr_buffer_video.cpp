#include "nr_buffer_video.h"

#include <Imath/half.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace dlss {
namespace {
void check(int rc, const char* operation) {
    if (rc >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, error, sizeof(error));
    throw std::runtime_error(std::string("NR buffer video: ") + operation + ": " + error);
}

const auto& motionQuantizationTable() {
    // A fixed 1/32-pixel grid caps the error at 1/64 pixel. Half floats with
    // magnitude >= 32 already fall on that grid. Canonicalize signed zero.
    static const auto table = [] {
        std::array<std::uint16_t, 1 << 16> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            Imath::half input;
            input.setBits(static_cast<std::uint16_t>(i));
            const float value = static_cast<float>(input);
            const float quantized = std::abs(value) < 32.f ? std::round(value * 32.f) / 32.f : value;
            out[i] = quantized == 0.f ? 0 : Imath::half(quantized).bits();
        }
        return out;
    }();
    return table;
}
}

struct NrBufferVideoWriter::Impl {
    std::filesystem::path target, temp;
    AVFormatContext* mux = nullptr;
    AVCodecContext* encoder = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    std::int64_t index = 0;

    ~Impl() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&encoder);
        if (mux) {
            if (mux->pb) avio_closep(&mux->pb);
            avformat_free_context(mux);
        }
        if (!temp.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
        }
    }

    void submit(AVFrame* data) {
        check(avcodec_send_frame(encoder, data), "send frame");
        for (;;) {
            const int rc = avcodec_receive_packet(encoder, packet);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            check(rc, "encode frame");
            // Derive the duration from adjacent PTS values: at 30 fps the
            // Matroska millisecond time base alternates 33 and 34 ticks.
            const auto pts = packet->pts;
            av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
            packet->duration = av_rescale_q(pts + 1, encoder->time_base, stream->time_base) - packet->pts;
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(mux, packet), "write frame");
        }
    }
};

NrBufferVideoWriter::NrBufferVideoWriter(const std::filesystem::path& path, int width, int height, int fps)
    : m(std::make_unique<Impl>()) {
    if (width <= 0 || height <= 0 || height > std::numeric_limits<int>::max() / kNrBufferPlanes || fps <= 0) {
        throw std::runtime_error("NR buffer video: invalid dimensions or rate");
    }
    m->target = path;
    m->temp = path;
    m->temp += ".part";
    const auto utf8 = m->temp.u8string();
    const auto* filename = reinterpret_cast<const char*>(utf8.c_str());
    check(avformat_alloc_output_context2(&m->mux, nullptr, "matroska", filename), "create Matroska muxer");
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (!codec) throw std::runtime_error("NR buffer video: FFV1 encoder unavailable");
    m->encoder = avcodec_alloc_context3(codec);
    m->stream = avformat_new_stream(m->mux, nullptr);
    m->frame = av_frame_alloc();
    m->packet = av_packet_alloc();
    if (!m->encoder || !m->stream || !m->frame || !m->packet) throw std::bad_alloc();

    auto* enc = m->encoder;
    enc->width = width;
    enc->height = height * kNrBufferPlanes;
    enc->pix_fmt = AV_PIX_FMT_GRAY16LE;
    enc->bits_per_raw_sample = 16;
    enc->time_base = {1, fps};
    enc->framerate = {fps, 1};
    enc->level = 3;
    enc->gop_size = 1;
    enc->thread_count = 4;
    enc->thread_type = FF_THREAD_SLICE;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(enc, codec, nullptr), "open FFV1 encoder");
    check(avcodec_parameters_from_context(m->stream->codecpar, enc), "copy FFV1 parameters");
    m->stream->time_base = enc->time_base;
    m->stream->avg_frame_rate = enc->framerate;
    check(av_dict_set(&m->stream->metadata, "title", kNrBufferTrackTitle, 0), "write guide layout tag");

    check(avio_open(&m->mux->pb, filename, AVIO_FLAG_WRITE), "open output");
    check(avformat_write_header(m->mux, nullptr), "write header");
    m->frame->format = enc->pix_fmt;
    m->frame->width = width;
    m->frame->height = enc->height;
    check(av_frame_get_buffer(m->frame, 0), "allocate frame");
}

NrBufferVideoWriter::~NrBufferVideoWriter() = default;

void NrBufferVideoWriter::write(const std::vector<std::uint8_t>& motion, const std::vector<std::uint8_t>& depth) {
    const auto width = m->frame->width;
    const auto height = m->frame->height / kNrBufferPlanes;
    const auto bytes = static_cast<std::size_t>(width) * height * 4;
    if (motion.size() != bytes || depth.size() != bytes) throw std::runtime_error("NR buffer video: invalid guide size");
    check(av_frame_make_writable(m->frame), "make frame writable");
    const auto* mv = reinterpret_cast<const std::uint16_t*>(motion.data());
    const auto* z = reinterpret_cast<const float*>(depth.data());
    const auto& quantized = motionQuantizationTable();
    for (int y = 0; y < height; ++y) {
        auto row = [this, y, height](int plane) {
            return reinterpret_cast<std::uint16_t*>(m->frame->data[0] + (y + plane * height) * m->frame->linesize[0]);
        };
        auto* xPlane = row(0);
        auto* yPlane = row(1);
        auto* zPlane = row(2);
        for (int x = 0; x < width; ++x) {
            const auto i = static_cast<std::size_t>(y) * width + x;
            xPlane[x] = quantized[mv[2 * i]];
            yPlane[x] = quantized[mv[2 * i + 1]];
            zPlane[x] = Imath::half(z[i]).bits();
        }
    }
    m->frame->pts = m->index++;
    m->submit(m->frame);
}

void NrBufferVideoWriter::close() {
    m->submit(nullptr);
    check(av_write_trailer(m->mux), "write trailer");
    check(avio_closep(&m->mux->pb), "close output");
    std::filesystem::rename(m->temp, m->target);
    m->temp.clear();
}

} // namespace dlss
