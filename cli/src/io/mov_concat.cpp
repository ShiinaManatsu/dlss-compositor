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
        const auto utf8 = file.u8string();
        int err = avformat_open_input(&fmt, reinterpret_cast<const char*>(utf8.c_str()), nullptr, nullptr);
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
    if (a->codec_id == AV_CODEC_ID_FFV1 && a->extradata_size > 0 &&
        std::memcmp(a->extradata, b->extradata, a->extradata_size) != 0)
        fail("segment " + std::to_string(index) + " FFV1 settings mismatch");
    // Timings must line up too — every packet is stamped with segment 0's
    // frame period (see ticksPerFrame below).
    AVRational fa = first.stream->avg_frame_rate;
    AVRational fb = next.stream->avg_frame_rate;
    if (fa.num <= 0) fa = first.stream->r_frame_rate;
    if (fb.num <= 0) fb = next.stream->r_frame_rate;
    if (av_cmp_q(fa, fb) != 0)
        fail("segment " + std::to_string(index) + " frame rate mismatch");
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

        // Muxer inferred from the output extension (.mov, .mp4, or guide .mkv).
        AVFormatContext* out = nullptr;
        const auto utf8 = output.u8string();
        const auto* outName = reinterpret_cast<const char*>(utf8.c_str());
        int err = avformat_alloc_output_context2(&out, nullptr, nullptr, outName);
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
        const bool guideVideo = outStream->codecpar->codec_id == AV_CODEC_ID_FFV1;
        if (guideVideo) av_dict_copy(&outStream->metadata, segs[0].stream->metadata, 0);

        if (!(out->oformat->flags & AVFMT_NOFILE)) {
            err = avio_open(&out->pb, outName, AVIO_FLAG_WRITE);
            if (err < 0) { avformat_free_context(out); fail("cannot open output: " + fferr(err)); }
        }

        // Match the writer's fragmented MOV/MP4 (crash-safe moof fragments);
        // write_colr carries the copied codecpar colors into a colr box.
        AVDictionary* muxOptions = nullptr;
        if (!guideVideo) av_dict_set(&muxOptions, "movflags", "+write_colr+frag_keyframe+empty_moov+default_base_moof", 0);
        err = avformat_write_header(out, &muxOptions);
        av_dict_free(&muxOptions);
        if (err < 0) { avio_closep(&out->pb); avformat_free_context(out); fail("cannot write output header: " + fferr(err)); }

        std::int64_t frames = 0;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) { avio_closep(&out->pb); avformat_free_context(out); fail("cannot allocate packet"); }
        // Stamp every packet with the frame period (in the OUTPUT track time
        // base). Fragmented MOVs carry sample durations in tfhd/trun and movenc
        // passes pkt->duration through verbatim — never rescales — so this is
        // the same rule the prores writer follows. Packets read back from a
        // fragmented MOV have no usable duration; without the stamp samples
        // land at 1 tick, the demuxer reports the tb-echo frame rate plus a
        // skewed avg, and CFR seeking drifts onto the wrong frame.
        AVRational fr = outStream->avg_frame_rate;
        if (fr.num <= 0) fr = outStream->r_frame_rate;
        const std::int64_t ticksPerFrame =
            fr.num > 0 ? av_rescale_q(1, av_inv_q(fr), outStream->time_base) : 0;
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
            int readResult;
            while ((readResult = av_read_frame(segs[i].fmt, pkt)) >= 0) {
                if (pkt->stream_index != segs[i].videoStream) {
                    av_packet_unref(pkt);
                    continue;
                }
                pkt->stream_index = outStream->index;
                av_packet_rescale_ts(pkt, srcTb, outStream->time_base);
                if (ticksPerFrame > 0) pkt->duration = ticksPerFrame;
                if (pkt->pts != AV_NOPTS_VALUE) {
                    if (firstPts == AV_NOPTS_VALUE) firstPts = pkt->pts;
                    lastPts = pkt->pts;
                    pkt->pts += tsOffset;
                }
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += tsOffset;
                if (guideVideo && fr.num > 0) {
                    // Every guide frame is independently coded. Generate CFR
                    // timestamps from the absolute index to avoid accumulating
                    // millisecond rounding at each segment boundary.
                    pkt->pts = pkt->dts = av_rescale_q(frames, av_inv_q(fr), outStream->time_base);
                    pkt->duration = av_rescale_q(frames + 1, av_inv_q(fr), outStream->time_base) - pkt->pts;
                }
                if (pkt->duration > 0) lastDur = pkt->duration;
                err = av_interleaved_write_frame(out, pkt);
                if (err < 0) {
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    avio_closep(&out->pb);
                    avformat_free_context(out);
                    fail("write failed on segment " + std::to_string(i) + ": " + fferr(err));
                }
                ++frames;
                ++segFrames;
            }
            if (readResult != AVERROR_EOF) {
                av_packet_free(&pkt);
                avio_closep(&out->pb);
                avformat_free_context(out);
                fail("read failed on segment " + std::to_string(i) + ": " + fferr(readResult));
            }
            if (ticksPerFrame > 0) {
                tsOffset += segFrames * ticksPerFrame;
            } else if (firstPts >= 0 && lastPts >= 0) {
                // No frame rate anywhere — fall back to the observed pts span.
                tsOffset += (lastPts - firstPts) + (lastDur > 0 ? lastDur : 1);
            }
            LOG_INFO("[MovConcat] segment {}: {} frames, +{} ticks offset now {}",
                     i, segFrames, tsOffset, tsOffset);
        }
        av_packet_free(&pkt);

        err = av_write_trailer(out);
        if (err < 0) {
            avio_closep(&out->pb);
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
