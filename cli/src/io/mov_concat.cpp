// mov_concat — see mov_concat.h.
//
// FFmpeg is linked statically via vcpkg (same libs prores_writer uses); its
// public headers drop the extern "C" guard — wrap manually.

#include "mov_concat.h"

#include "../core/log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <cstring>
#include <stdexcept>

namespace dlss {

namespace {

std::string fferr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("MovConcat: " + what);
}

/// One opened segment: format context + the chosen video stream index.
struct SegmentInput {
    AVFormatContext* fmt = nullptr;
    int videoStream = -1;
    AVStream* stream = nullptr;

    void open(const std::filesystem::path& file) {
        int err = avformat_open_input(&fmt, file.string().c_str(), nullptr, nullptr);
        if (err < 0 || !fmt) fail("cannot open " + file.string() + ": " + fferr(err));
        err = avformat_find_stream_info(fmt, nullptr);
        if (err < 0) fail("cannot read stream info: " + file.string() + ": " + fferr(err));
        videoStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0) fail("no video stream: " + file.string());
        stream = fmt->streams[videoStream];
    }

    void close() {
        if (fmt) avformat_close_input(&fmt);
        fmt = nullptr;
        stream = nullptr;
        videoStream = -1;
    }
};

void verifyCompat(const SegmentInput& first, const SegmentInput& next, std::size_t index) {
    const AVCodecParameters* a = first.stream->codecpar;
    const AVCodecParameters* b = next.stream->codecpar;
    if (a->codec_id != b->codec_id)
        fail("segment " + std::to_string(index) + " codec mismatch");
    if (a->width != b->width || a->height != b->height)
        fail("segment " + std::to_string(index) + " resolution mismatch");
    if (a->format != b->format)
        fail("segment " + std::to_string(index) + " pixel format mismatch");
    if (a->extradata_size != b->extradata_size)
        fail("segment " + std::to_string(index) + " extradata size mismatch (different encoder settings?)");
}

} // namespace

std::int64_t concatVideoFiles(const std::vector<std::filesystem::path>& inputs,
                              const std::filesystem::path& output) {
    if (inputs.empty()) fail("no input files");
    if (inputs.size() == 1)
        fail("concat needs at least 2 inputs; copy the file instead");

    std::vector<SegmentInput> segs(inputs.size());
    try {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!std::filesystem::is_regular_file(inputs[i]))
                fail("missing segment file " + inputs[i].string());
            segs[i].open(inputs[i]);
            if (i > 0) verifyCompat(segs[0], segs[i], i);
        }

        // Muxer inferred from the output extension (.mov / .mp4 — the same
        // containers our ProRes/H265 writers produce).
        AVFormatContext* out = nullptr;
        int err = avformat_alloc_output_context2(&out, nullptr, nullptr, output.string().c_str());
        if (err < 0 || !out)
            fail("cannot create output muxer for " + output.string() + ": " + fferr(err));

        AVStream* outStream = avformat_new_stream(out, nullptr);
        if (!outStream) { avformat_free_context(out); fail("cannot allocate output stream"); }
        err = avcodec_parameters_copy(outStream->codecpar, segs[0].stream->codecpar);
        if (err < 0) { avformat_free_context(out); fail("cannot copy codec parameters: " + fferr(err)); }
        outStream->codecpar->codec_tag = 0;
        // Same track time base as the writer ({1,15360} for our ProRes MOVs) —
        // per-sample durations land on the true frame period, so demuxers see
        // the real frame rate instead of the tick-rate echo (see prores_writer).
        outStream->time_base = segs[0].stream->time_base;
        outStream->avg_frame_rate = segs[0].stream->avg_frame_rate;
        outStream->r_frame_rate = segs[0].stream->r_frame_rate;

        if (!(out->oformat->flags & AVFMT_NOFILE)) {
            err = avio_open(&out->pb, output.string().c_str(), AVIO_FLAG_WRITE);
            if (err < 0) { avformat_free_context(out); fail("cannot open output: " + fferr(err)); }
        }

        // Match the writer's fragmented MOV/MP4 (crash-safe moof fragments);
        // write_colr carries the copied codecpar colors into a colr box.
        AVDictionary* muxOptions = nullptr;
        av_dict_set(&muxOptions, "movflags", "+write_colr+frag_keyframe+empty_moov+default_base_moof", 0);
        err = avformat_write_header(out, &muxOptions);
        av_dict_free(&muxOptions);
        if (err < 0) { avformat_free_context(out); fail("cannot write output header: " + fferr(err)); }

        std::int64_t frames = 0;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) { avformat_free_context(out); fail("cannot allocate packet"); }
        // Each segment's own timeline starts at PTS 0 — writing them back to
        // back would rewind DTS and the muxer rejects that (EINVAL). Shift every
        // packet by the accumulated duration of all previous segments (in the
        // OUTPUT stream time base, applied after rescaling).
        std::int64_t tsOffset = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const AVRational srcTb = segs[i].stream->time_base;
            std::int64_t firstPts = AV_NOPTS_VALUE;
            std::int64_t lastPts = AV_NOPTS_VALUE;
            std::int64_t lastDur = 0;
            std::int64_t segFrames = 0;
            while (av_read_frame(segs[i].fmt, pkt) >= 0) {
                if (pkt->stream_index != segs[i].videoStream) {
                    av_packet_unref(pkt);
                    continue;
                }
                pkt->stream_index = outStream->index;
                av_packet_rescale_ts(pkt, srcTb, outStream->time_base);
                if (pkt->pts != AV_NOPTS_VALUE) {
                    if (firstPts == AV_NOPTS_VALUE) firstPts = pkt->pts;
                    lastPts = pkt->pts;
                    pkt->pts += tsOffset;
                }
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += tsOffset;
                if (pkt->duration > 0) lastDur = pkt->duration;
                err = av_write_frame(out, pkt);  // takes ownership of pkt's refs
                if (err < 0) {
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    avformat_free_context(out);
                    fail("write failed on segment " + std::to_string(i) + ": " + fferr(err));
                }
                ++frames;
                ++segFrames;
            }
            if (firstPts >= 0 && lastPts >= 0) {
                // Fragmented MOVs carry no per-packet duration (ffprobe shows
                // N/A) — derive the frame period from the frame rate instead:
                // advance = frames * (1/fps) in output tb ticks.
                AVRational fr = outStream->avg_frame_rate;
                if (fr.num <= 0) fr = outStream->r_frame_rate;
                std::int64_t ticksPerFrame = fr.num > 0
                    ? av_rescale_q(1, av_inv_q(fr), outStream->time_base)
                    : lastDur;
                if (ticksPerFrame <= 0) ticksPerFrame = (lastPts - firstPts) + 1;
                tsOffset += segFrames * ticksPerFrame;
            }
            LOG_INFO("[MovConcat] segment {}: {} frames, +{} ticks offset now {}",
                     i, segFrames, tsOffset, tsOffset);
        }
        av_packet_free(&pkt);

        err = av_write_trailer(out);
        if (err < 0) {
            avformat_free_context(out);
            fail("cannot write trailer: " + fferr(err));
        }
        if (!(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
        avformat_free_context(out);

        for (auto& seg : segs) seg.close();
        LOG_INFO("[MovConcat] {} segment(s) -> {} ({} frames)", segs.size(), output.string(), frames);
        return frames;
    } catch (...) {
        for (auto& seg : segs) seg.close();
        throw;
    }
}

} // namespace dlss
