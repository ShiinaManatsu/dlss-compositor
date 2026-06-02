// =============================================================================
// FramePrefetcher implementation.
// =============================================================================

#include "frame_prefetcher.h"

#include "../core/log.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <regex>
#include <utility>

namespace dlss {

namespace {

// ---------------------------------------------------------------------------
// Extract numeric frame index from filename (e.g. "0042.exr" -> 42).
// Returns -1 if no leading digit run is parseable.
// ---------------------------------------------------------------------------
int extractIndex(const std::filesystem::path& path)
{
    const std::string stem = path.stem().string();

    // Find the longest *trailing* run of digits — handles "frame_0042" too.
    std::size_t start = stem.size();
    while (start > 0 && std::isdigit(static_cast<unsigned char>(stem[start - 1]))) {
        --start;
    }
    if (start == stem.size()) {
        // No trailing digits — try whole-stem parse for plain "0042" case.
        try {
            return std::stoi(stem);
        } catch (...) {
            return -1;
        }
    }
    try {
        return std::stoi(stem.substr(start));
    } catch (...) {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Synchronously decode one EXR file into a DecodedFrame.
// All exceptions are caught and converted to ReadError.
// ---------------------------------------------------------------------------
FrameResult decodeExr(const std::filesystem::path& filePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || ec) {
        return std::unexpected(ReadError{
            ReadErrorCode::FileNotFound,
            "File not found: " + filePath.string(),
            filePath});
    }

    try {
        Imf::InputFile      file(filePath.string().c_str());
        const Imf::Header&  header = file.header();
        const Imath::Box2i  dw     = header.dataWindow();

        const int width  = dw.max.x - dw.min.x + 1;
        const int height = dw.max.y - dw.min.y + 1;
        if (width <= 0 || height <= 0) {
            return std::unexpected(ReadError{
                ReadErrorCode::ParseFailed,
                "Non-positive data window in " + filePath.string(),
                filePath});
        }
        const std::size_t pixels = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);

        DecodedFrame frame;
        frame.width       = static_cast<std::uint32_t>(width);
        frame.height      = static_cast<std::uint32_t>(height);
        frame.source_path = filePath;
        frame.combined.assign(pixels * 4, 0.0f);
        frame.depth.assign(pixels, 0.0f);
        frame.vector.assign(pixels * 4, 0.0f);

        const Imf::ChannelList& channels = header.channels();
        const bool hasZ        = channels.findChannel("Z")        != nullptr;
        const bool hasVectorX  = channels.findChannel("Vector.X") != nullptr;

        if (!hasZ) {
            return std::unexpected(ReadError{
                ReadErrorCode::MissingChannel,
                "Missing Z (depth) channel in " + filePath.string(),
                filePath});
        }

        // Standard OpenEXR base-pointer trick: subtract data-window origin so
        // that file pixel (x,y) lands on buffer index (x - dw.min.x, y - dw.min.y).
        const std::size_t pixel_stride_rgba = 4u * sizeof(float);
        const std::size_t row_stride_rgba   =
            pixel_stride_rgba * static_cast<std::size_t>(width);
        const std::size_t pixel_stride_z = sizeof(float);
        const std::size_t row_stride_z   =
            pixel_stride_z * static_cast<std::size_t>(width);

        const std::ptrdiff_t origin_byte_offset_rgba =
            static_cast<std::ptrdiff_t>(dw.min.x) *
                static_cast<std::ptrdiff_t>(pixel_stride_rgba) +
            static_cast<std::ptrdiff_t>(dw.min.y) *
                static_cast<std::ptrdiff_t>(row_stride_rgba);

        const std::ptrdiff_t origin_byte_offset_z =
            static_cast<std::ptrdiff_t>(dw.min.x) *
                static_cast<std::ptrdiff_t>(pixel_stride_z) +
            static_cast<std::ptrdiff_t>(dw.min.y) *
                static_cast<std::ptrdiff_t>(row_stride_z);

        Imf::FrameBuffer fb;

        // Combined RGBA — interleaved layout.
        const char* const rgba_names[4] = {"R", "G", "B", "A"};
        for (int ch = 0; ch < 4; ++ch) {
            char* base = reinterpret_cast<char*>(frame.combined.data() + ch);
            base -= origin_byte_offset_rgba;
            fb.insert(rgba_names[ch],
                      Imf::Slice(Imf::FLOAT, base,
                                 pixel_stride_rgba, row_stride_rgba));
        }

        // Depth (single Z channel, planar).
        {
            char* base = reinterpret_cast<char*>(frame.depth.data());
            base -= origin_byte_offset_z;
            fb.insert("Z",
                      Imf::Slice(Imf::FLOAT, base,
                                 pixel_stride_z, row_stride_z));
        }

        // Motion vector (Cycles default: Vector.X / .Y / .Z / .W) — optional.
        if (hasVectorX) {
            const char* const vec_names[4] = {
                "Vector.X", "Vector.Y", "Vector.Z", "Vector.W"};
            for (int ch = 0; ch < 4; ++ch) {
                if (channels.findChannel(vec_names[ch]) == nullptr) {
                    continue;
                }
                char* base = reinterpret_cast<char*>(frame.vector.data() + ch);
                base -= origin_byte_offset_rgba;
                fb.insert(vec_names[ch],
                          Imf::Slice(Imf::FLOAT, base,
                                     pixel_stride_rgba, row_stride_rgba));
            }
        }

        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        return frame;

    } catch (const std::exception& e) {
        return std::unexpected(ReadError{
            ReadErrorCode::ParseFailed,
            std::string("Failed to parse EXR: ") + e.what(),
            filePath});
    } catch (...) {
        return std::unexpected(ReadError{
            ReadErrorCode::ParseFailed,
            "Unknown error parsing EXR",
            filePath});
    }
}

} // namespace

// =============================================================================
// FramePrefetcher
// =============================================================================

FramePrefetcher::FramePrefetcher(std::vector<std::filesystem::path> framePaths,
                                 int prefetchDepth)
    : m_paths(std::move(framePaths))
    , m_prefetchDepth(std::max(1, prefetchDepth))
{
    m_worker = std::thread([this] { workerLoop(); });
}

FramePrefetcher::~FramePrefetcher()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void FramePrefetcher::workerLoop()
{
    const int total = static_cast<int>(m_paths.size());
    for (int idx = 0; idx < total; ++idx) {
        // Backpressure — sleep if we are `prefetchDepth` ahead of the consumer.
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] {
                return m_stop.load() ||
                       (m_produced.load() - m_consumed.load()) < m_prefetchDepth;
            });
            if (m_stop.load()) {
                return;
            }
        }

        FrameResult result = decodeExr(m_paths[idx]);

        // Gap detection — non-sequential numeric stems force a reset on the
        // *current* frame so the next frame can be marked is_first internally.
        if (result.has_value() && idx + 1 < total) {
            const int thisIdx = extractIndex(m_paths[idx]);
            const int nextIdx = extractIndex(m_paths[idx + 1]);
            if (thisIdx >= 0 && nextIdx >= 0 && (nextIdx - thisIdx) > 1) {
                result->has_gap_after = true;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(result));
            m_produced.fetch_add(1);
        }
        m_cv.notify_all();
    }
}

FrameResult FramePrefetcher::getNext()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&] {
        return !m_queue.empty() || m_stop.load();
    });

    if (m_queue.empty()) {
        return std::unexpected(ReadError{
            ReadErrorCode::IoError,
            "Prefetcher queue empty (stopped or no more frames)",
            {}});
    }

    FrameResult result = std::move(m_queue.front());
    m_queue.pop();
    m_consumed.fetch_add(1);
    lock.unlock();
    m_cv.notify_all(); // Wake worker — there's now room in the prefetch window.
    return result;
}

} // namespace dlss
