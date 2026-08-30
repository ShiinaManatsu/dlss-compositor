// =============================================================================
// FramePrefetcher implementation.
// =============================================================================

#include "frame_prefetcher.h"

#include "../core/log.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace dlss {

namespace {

constexpr int kMaxExrDimension = 16384;
constexpr std::size_t kMaxExrPixels = 16384ull * 16384ull;
constexpr int kMaxExrParts = 64;

struct ImageLayout {
    int width = 0;
    int height = 0;
    std::size_t pixels = 0;
    std::size_t pixelStrideRgba = 0;
    std::size_t rowStrideRgba = 0;
    std::size_t pixelStrideZ = 0;
    std::size_t rowStrideZ = 0;
    std::ptrdiff_t originByteOffsetRgba = 0;
    std::ptrdiff_t originByteOffsetZ = 0;
};

ImageLayout makeLayout(const Imath::Box2i& dw)
{
    ImageLayout layout;
    const long long width = static_cast<long long>(dw.max.x) -
                            static_cast<long long>(dw.min.x) + 1ll;
    const long long height = static_cast<long long>(dw.max.y) -
                             static_cast<long long>(dw.min.y) + 1ll;
    if (width > static_cast<long long>(std::numeric_limits<int>::max()) ||
        height > static_cast<long long>(std::numeric_limits<int>::max())) {
        return layout;
    }

    layout.width  = static_cast<int>(width);
    layout.height = static_cast<int>(height);
    layout.pixels = static_cast<std::size_t>(layout.width) *
                    static_cast<std::size_t>(layout.height);

    layout.pixelStrideRgba = 4u * sizeof(float);
    layout.rowStrideRgba   = layout.pixelStrideRgba * static_cast<std::size_t>(layout.width);
    layout.pixelStrideZ    = sizeof(float);
    layout.rowStrideZ      = layout.pixelStrideZ * static_cast<std::size_t>(layout.width);

    layout.originByteOffsetRgba =
        static_cast<std::ptrdiff_t>(dw.min.x) *
            static_cast<std::ptrdiff_t>(layout.pixelStrideRgba) +
        static_cast<std::ptrdiff_t>(dw.min.y) *
            static_cast<std::ptrdiff_t>(layout.rowStrideRgba);
    layout.originByteOffsetZ =
        static_cast<std::ptrdiff_t>(dw.min.x) *
            static_cast<std::ptrdiff_t>(layout.pixelStrideZ) +
        static_cast<std::ptrdiff_t>(dw.min.y) *
            static_cast<std::ptrdiff_t>(layout.rowStrideZ);

    return layout;
}

bool hasChannel(const Imf::ChannelList& channels, const char* name)
{
    return channels.findChannel(name) != nullptr;
}

std::string firstChannelName(const Imf::ChannelList& channels)
{
    Imf::ChannelList::ConstIterator it = channels.begin();
    return it == channels.end() ? std::string{} : std::string{it.name()};
}

void insertRgbaSlices(Imf::FrameBuffer& fb, DecodedFrame& frame, const ImageLayout& layout)
{
    const char* const rgbaNames[4] = {"R", "G", "B", "A"};
    for (int ch = 0; ch < 4; ++ch) {
        char* base = reinterpret_cast<char*>(frame.combined.data() + ch);
        base -= layout.originByteOffsetRgba;
        fb.insert(rgbaNames[ch],
                  Imf::Slice(Imf::FLOAT, base,
                             layout.pixelStrideRgba, layout.rowStrideRgba));
    }
}

void insertDepthSlice(Imf::FrameBuffer& fb,
                      DecodedFrame& frame,
                      const ImageLayout& layout,
                      const char* channelName)
{
    char* base = reinterpret_cast<char*>(frame.depth.data());
    base -= layout.originByteOffsetZ;
    fb.insert(channelName,
              Imf::Slice(Imf::FLOAT, base,
                         layout.pixelStrideZ, layout.rowStrideZ));
}

void insertVectorSlices(Imf::FrameBuffer& fb,
                        DecodedFrame& frame,
                        const ImageLayout& layout,
                        const Imf::ChannelList& channels)
{
    const char* const vecNames[4] = {
        "Vector.X", "Vector.Y", "Vector.Z", "Vector.W"};
    for (int ch = 0; ch < 4; ++ch) {
        if (!hasChannel(channels, vecNames[ch])) {
            continue;
        }
        char* base = reinterpret_cast<char*>(frame.vector.data() + ch);
        base -= layout.originByteOffsetRgba;
        fb.insert(vecNames[ch],
                  Imf::Slice(Imf::FLOAT, base,
                             layout.pixelStrideRgba, layout.rowStrideRgba));
    }
}

void insertNamedVectorSlices(Imf::FrameBuffer& fb,
                             DecodedFrame& frame,
                             const ImageLayout& layout,
                             const Imf::ChannelList& channels,
                             const char* const channelNames[4])
{
    for (int ch = 0; ch < 4; ++ch) {
        if (!hasChannel(channels, channelNames[ch])) {
            continue;
        }
        char* base = reinterpret_cast<char*>(frame.vector.data() + ch);
        base -= layout.originByteOffsetRgba;
        fb.insert(channelNames[ch],
                  Imf::Slice(Imf::FLOAT, base,
                             layout.pixelStrideRgba, layout.rowStrideRgba));
    }
}

bool isVectorPartName(const std::string& partName)
{
    return partName == "Vector" || partName == "vector";
}

bool readSinglePart(Imf::InputFile& file,
                    const Imath::Box2i& dw,
                    DecodedFrame& frame,
                    const ImageLayout& layout)
{
    const Imf::Header& header = file.header();
    const Imf::ChannelList& channels = header.channels();
    if (!hasChannel(channels, "R") || !hasChannel(channels, "G") ||
        !hasChannel(channels, "B") || !hasChannel(channels, "A")) {
        return false;
    }

    Imf::FrameBuffer fb;
    insertRgbaSlices(fb, frame, layout);
    if (hasChannel(channels, "Z")) {
        insertDepthSlice(fb, frame, layout, "Z");
    }
    insertVectorSlices(fb, frame, layout, channels);

    file.setFrameBuffer(fb);
    file.readPixels(dw.min.y, dw.max.y);
    return true;
}

bool readMultipart(const std::filesystem::path& filePath,
                   const Imath::Box2i& dw,
                   DecodedFrame& frame,
                   const ImageLayout& layout)
{
    Imf::MultiPartInputFile multi(filePath.string().c_str());
    if (multi.parts() <= 1) {
        return false;
    }
    if (multi.parts() > kMaxExrParts) {
        throw std::runtime_error("EXR has too many parts: " + filePath.string());
    }

    bool readColor = false;
    bool readDepth = false;
    bool readVector = false;

    for (int partIndex = 0; partIndex < multi.parts(); ++partIndex) {
        Imf::InputPart part(multi, partIndex);
        const Imf::Header& header = part.header();
        const Imf::ChannelList& channels = header.channels();
        const std::string partName = header.hasName() ? header.name() : std::string{};
        const Imath::Box2i partDw = header.dataWindow();
        if (partDw != dw) {
            LOG_WARN("[FramePrefetcher] Skipping EXR part '{}' with mismatched dataWindow in {}",
                     partName, filePath.string());
            continue;
        }

        Imf::FrameBuffer fb;
        bool shouldRead = false;

        if (!readColor && hasChannel(channels, "R") && hasChannel(channels, "G") &&
            hasChannel(channels, "B") && hasChannel(channels, "A")) {
            insertRgbaSlices(fb, frame, layout);
            readColor = true;
            shouldRead = true;
        }

        if (!readDepth) {
            if (hasChannel(channels, "Z")) {
                insertDepthSlice(fb, frame, layout, "Z");
                readDepth = true;
                shouldRead = true;
            } else if (partName == "Z") {
                const std::string channelName = firstChannelName(channels);
                if (!channelName.empty()) {
                    insertDepthSlice(fb, frame, layout, channelName.c_str());
                    readDepth = true;
                    shouldRead = true;
                }
            }
        }

        if (!readVector) {
            if (hasChannel(channels, "Vector.X")) {
                insertVectorSlices(fb, frame, layout, channels);
                readVector = true;
                shouldRead = true;
            } else if (isVectorPartName(partName) && hasChannel(channels, "X")) {
                const char* const rawVecNames[4] = {"X", "Y", "Z", "W"};
                insertNamedVectorSlices(fb, frame, layout, channels, rawVecNames);
                readVector = true;
                shouldRead = true;
            }
        }

        if (shouldRead) {
            part.setFrameBuffer(fb);
            part.readPixels(dw.min.y, dw.max.y);
        }
        if (readColor && readDepth && readVector) {
            break;
        }
    }

    LOG_DEBUG("[FramePrefetcher] EXR {} parts={} color={} depth={} vector={}",
              filePath.string(), multi.parts(), readColor, readDepth, readVector);
    return readColor;
}

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

        const ImageLayout layout = makeLayout(dw);
        if (layout.width <= 0 || layout.height <= 0) {
            return std::unexpected(ReadError{
                ReadErrorCode::ParseFailed,
                "Non-positive data window in " + filePath.string(),
                filePath});
        }
        if (layout.width > kMaxExrDimension || layout.height > kMaxExrDimension ||
            layout.pixels > kMaxExrPixels) {
            return std::unexpected(ReadError{
                ReadErrorCode::ParseFailed,
                "EXR data window exceeds supported size limit: " + filePath.string(),
                filePath});
        }

        DecodedFrame frame;
        frame.width       = static_cast<std::uint32_t>(layout.width);
        frame.height      = static_cast<std::uint32_t>(layout.height);
        frame.source_path = filePath;
        frame.combined.assign(layout.pixels * 4, 0.0f);
        frame.depth.assign(layout.pixels, 1.0f);
        frame.vector.assign(layout.pixels * 4, 0.0f);

        if (!readMultipart(filePath, dw, frame, layout) && !readSinglePart(file, dw, frame, layout)) {
            return std::unexpected(ReadError{
                ReadErrorCode::MissingChannel,
                "EXR is missing Combined RGBA channels: " + filePath.string(),
                filePath});
        }

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
                                 int prefetchDepth,
                                 bool waitForFiles,
                                 const std::atomic<bool>* externalStop)
    : m_paths(std::move(framePaths))
    , m_prefetchDepth(std::max(1, prefetchDepth))
    , m_waitForFiles(waitForFiles)
    , m_externalStop(externalStop)
{
    if (m_waitForFiles) {
        LOG_INFO("[FramePrefetcher] wait-for-files mode (watch input)");
    }
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
                return stopRequested() ||
                       (m_produced.load() - m_consumed.load()) < m_prefetchDepth;
            });
            if (stopRequested()) {
                return;
            }
        }

        FrameResult result = [&] {
            if (m_waitForFiles) {
                // Watch mode: Blender streams the EXR; wait for existence and a
                // stable size so a partially-written file is never decoded.
                constexpr auto kTick        = std::chrono::milliseconds(100);
                constexpr int  kStableTicks = 3;
                std::uintmax_t lastSize     = 0;
                int            stable       = 0;
                while (!stopRequested()) {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(m_paths[idx], ec);
                    if (!ec && size > 0 && size == lastSize) {
                        if (++stable >= kStableTicks) break;
                    } else {
                        stable   = 0;
                        lastSize = ec ? 0 : size;
                    }
                    std::this_thread::sleep_for(kTick);
                }
                if (stopRequested()) {
                    return FrameResult(std::unexpected(ReadError{ReadErrorCode::IoError,
                        "stopped while waiting for file", m_paths[idx]}));
                }
            }
            return decodeExr(m_paths[idx]);
        }();

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

FrameResult FramePrefetcher::decodeFile(const std::filesystem::path& path) {
    return decodeExr(path);
}

FrameResult FramePrefetcher::getNext()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&] {
        return !m_queue.empty() || stopRequested();
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
