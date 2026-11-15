// RrFramePrefetcher implementation.
// Reads Blender 5.3 RR multilayer EXR files; decodes all guide buffer channels.

#include "rr_frame_prefetcher.h"
#include "../core/log.h"
#include "../core/path_encoding.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfThreading.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace dlss {

namespace {

constexpr int kMaxDim    = 16384;
constexpr std::size_t kMaxPixels = 16384ull * 16384ull;

// ---------------------------------------------------------------------------
// Blender 5.3 RR channel names (inject.py RR_FILE_ITEMS + core buffers).
// ---------------------------------------------------------------------------
// Combined / beauty. Blender 5 File Output empty-name slot writes R/G/B/A
// (OIIO) rather than Image.R; older inject used Image.* ; Combined.* also seen.
static constexpr const char* kColorChannelsImage[4]    = {"Image.R","Image.G","Image.B","Image.A"};
static constexpr const char* kColorChannelsRGBA[4]     = {"R","G","B","A"};
static constexpr const char* kColorChannelsCombined[4] = {"Combined.R","Combined.G","Combined.B","Combined.A"};
// Denoising Depth
static constexpr const char* kDepthChannel        = "Denoising Depth.V";
// Vector (motion vectors)
static constexpr const char* kMotionChannels[2]   = {"Vector.X","Vector.Y"};
// Denoising Albedo
static constexpr const char* kDiffChannels[4]     = {
    "Denoising Albedo.R","Denoising Albedo.G","Denoising Albedo.B","Denoising Albedo.A"};
// Denoising Specular Albedo
static constexpr const char* kSpecChannels[4]     = {
    "Denoising Specular Albedo.R","Denoising Specular Albedo.G",
    "Denoising Specular Albedo.B","Denoising Specular Albedo.A"};
// Denoising Normal
static constexpr const char* kNormChannels[3]     = {
    "Denoising Normal.X","Denoising Normal.Y","Denoising Normal.Z"};
// Denoising Roughness
static constexpr const char* kRoughChannel        = "Denoising Roughness.V";
// Denoising Specular Motion (Blender 5.3+)
static constexpr const char* kSpecMotionChannels[2] = {
    "Denoising Specular Motion.X","Denoising Specular Motion.Y"};
// Denoising Backward Motion (Blender 5.3+, stored for future use)
static constexpr const char* kBackMotionChannels[2] = {
    "Denoising Backward Motion.X","Denoising Backward Motion.Y"};

// ---------------------------------------------------------------------------

bool hasCh(const Imf::ChannelList& channels, const char* name) {
    return channels.findChannel(name) != nullptr;
}

struct Layout {
    int width  = 0;
    int height = 0;
    std::size_t pixels = 0;

    // stride helpers for interleaved (stride = channels * sizeof(float)) and scalar buffers
    std::ptrdiff_t originOffset(const Imath::Box2i& dw, std::size_t pixelStride) const {
        return static_cast<std::ptrdiff_t>(dw.min.x) * static_cast<std::ptrdiff_t>(pixelStride)
             + static_cast<std::ptrdiff_t>(dw.min.y) * static_cast<std::ptrdiff_t>(pixelStride * static_cast<std::size_t>(width));
    }
};

Layout makeLayout(const Imath::Box2i& dw) {
    Layout l;
    const long long w = (long long)dw.max.x - (long long)dw.min.x + 1ll;
    const long long h = (long long)dw.max.y - (long long)dw.min.y + 1ll;
    if (w < 1 || h < 1 || w > kMaxDim || h > kMaxDim) return l;
    l.width  = (int)w;
    l.height = (int)h;
    l.pixels = (std::size_t)l.width * (std::size_t)l.height;
    return l;
}

// Insert N interleaved float channels into a FrameBuffer.
// @param data       base pointer to the flat interleaved float array (stride = nCh floats)
// @param chNames    channel names, exactly nCh entries
// @param nCh        number of channels (channels are interleaved at offsets 0, 1, ... nCh-1)
void insertInterleavedSlices(Imf::FrameBuffer& fb,
                             float* data,
                             const char* const* chNames,
                             int nCh,
                             const Imf::ChannelList& channels,
                             const Layout& layout,
                             const Imath::Box2i& dw) {
    const std::size_t pixelStride = (std::size_t)nCh * sizeof(float);
    const std::size_t rowStride   = pixelStride * (std::size_t)layout.width;
    const std::ptrdiff_t originOff = layout.originOffset(dw, pixelStride);
    for (int ch = 0; ch < nCh; ++ch) {
        if (!hasCh(channels, chNames[ch])) continue;
        char* base = reinterpret_cast<char*>(data + ch) - originOff;
        fb.insert(chNames[ch], Imf::Slice(Imf::FLOAT, base, pixelStride, rowStride));
    }
}

// Insert a scalar float channel (stride = sizeof(float)).
void insertScalarSlice(Imf::FrameBuffer& fb,
                       float* data,
                       const char* chName,
                       const Imf::ChannelList& channels,
                       const Layout& layout,
                       const Imath::Box2i& dw) {
    if (!hasCh(channels, chName)) return;
    const std::size_t stride = sizeof(float);
    const std::ptrdiff_t originOff = layout.originOffset(dw, stride);
    char* base = reinterpret_cast<char*>(data) - originOff;
    fb.insert(chName, Imf::Slice(Imf::FLOAT, base, stride, stride * (std::size_t)layout.width));
}

int extractIndex(const std::filesystem::path& p) {
    const std::string stem = p.stem().string();
    std::size_t start = stem.size();
    while (start > 0 && std::isdigit((unsigned char)stem[start-1])) --start;
    if (start == stem.size()) {
        try { return std::stoi(stem); } catch (...) { return -1; }
    }
    try { return std::stoi(stem.substr(start)); } catch (...) { return -1; }
}

// ---------------------------------------------------------------------------
// Decode a single RR EXR file.
// ---------------------------------------------------------------------------
RrFrameResult decodeRrExr(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::unexpected(RrReadError{RrReadErrorCode::FileNotFound,
            "File not found: " + path.string(), path});
    }
    try {
        Imf::MultiPartInputFile multi(pathToUtf8(path).c_str());
        const int nParts = multi.parts();
        if (nParts < 1) {
            return std::unexpected(RrReadError{RrReadErrorCode::ParseFailed,
                "EXR has no parts: " + path.string(), path});
        }

        // Use data window from part 0.
        const Imath::Box2i dw = multi.header(0).dataWindow();
        const Layout layout = makeLayout(dw);
        if (layout.width <= 0 || layout.height <= 0 || layout.pixels > kMaxPixels) {
            return std::unexpected(RrReadError{RrReadErrorCode::ParseFailed,
                "EXR data window invalid or too large: " + path.string(), path});
        }

        RrDecodedFrame f;
        f.width        = (uint32_t)layout.width;
        f.height       = (uint32_t)layout.height;
        f.source_path  = path;

        f.colorRgba.assign(layout.pixels * 4, 0.0f);
        f.depthLinear.assign(layout.pixels, 1.0f);
        f.motionXy.assign(layout.pixels * 2, 0.0f);
        f.diffuseAlbedoRgba.assign(layout.pixels * 4, 0.0f);
        f.specAlbedoRgba.assign(layout.pixels * 4, 0.0f);
        f.normalsXyz.assign(layout.pixels * 3, 0.0f);
        f.roughness.assign(layout.pixels, 0.5f);
        f.specularMotionXy.assign(layout.pixels * 2, 0.0f);
        f.backwardMotionXy.assign(layout.pixels * 2, 0.0f);

        bool readColor = false, readDepth = false, readMotion = false;
        bool readDiffuse = false, readSpecAlbedo = false;
        bool readNormals = false, readRoughness = false;

        for (int p = 0; p < nParts; ++p) {
            Imf::InputPart part(multi, p);
            const Imf::ChannelList& channels = part.header().channels();

            Imf::FrameBuffer fb;
            bool insertedThisPart = false;

            if (!readColor) {
                const char* const* cols = nullptr;
                if (hasCh(channels, kColorChannelsImage[0]))         cols = kColorChannelsImage;
                else if (hasCh(channels, kColorChannelsRGBA[0]))     cols = kColorChannelsRGBA;
                else if (hasCh(channels, kColorChannelsCombined[0])) cols = kColorChannelsCombined;
                if (cols) {
                    insertInterleavedSlices(fb, f.colorRgba.data(), cols, 4, channels, layout, dw);
                    readColor = true;
                    insertedThisPart = true;
                }
            }
            if (!readDepth && hasCh(channels, kDepthChannel)) {
                insertScalarSlice(fb, f.depthLinear.data(), kDepthChannel, channels, layout, dw);
                readDepth = true;
                insertedThisPart = true;
            }
            if (!readMotion && hasCh(channels, "Vector.X")) {
                insertInterleavedSlices(fb, f.motionXy.data(), kMotionChannels, 2, channels, layout, dw);
                readMotion = true;
                insertedThisPart = true;
            }
            if (!readDiffuse && hasCh(channels, "Denoising Albedo.R")) {
                insertInterleavedSlices(fb, f.diffuseAlbedoRgba.data(), kDiffChannels, 4, channels, layout, dw);
                readDiffuse = true;
                insertedThisPart = true;
            }
            if (!readSpecAlbedo && hasCh(channels, "Denoising Specular Albedo.R")) {
                insertInterleavedSlices(fb, f.specAlbedoRgba.data(), kSpecChannels, 4, channels, layout, dw);
                readSpecAlbedo = true;
                insertedThisPart = true;
            }
            if (!readNormals && hasCh(channels, "Denoising Normal.X")) {
                insertInterleavedSlices(fb, f.normalsXyz.data(), kNormChannels, 3, channels, layout, dw);
                readNormals = true;
                insertedThisPart = true;
            }
            if (!readRoughness && hasCh(channels, kRoughChannel)) {
                insertScalarSlice(fb, f.roughness.data(), kRoughChannel, channels, layout, dw);
                readRoughness = true;
                insertedThisPart = true;
            }
            if (!f.hasSpecularMotion && hasCh(channels, kSpecMotionChannels[0]) && hasCh(channels, kSpecMotionChannels[1])) {
                insertInterleavedSlices(fb, f.specularMotionXy.data(), kSpecMotionChannels, 2, channels, layout, dw);
                f.hasSpecularMotion = true;
                insertedThisPart = true;
            }
            if (!f.hasBackwardMotion && hasCh(channels, kBackMotionChannels[0]) && hasCh(channels, kBackMotionChannels[1])) {
                insertInterleavedSlices(fb, f.backwardMotionXy.data(), kBackMotionChannels, 2, channels, layout, dw);
                f.hasBackwardMotion = true;
                insertedThisPart = true;
            }

            if (!insertedThisPart) continue;
            part.setFrameBuffer(fb);
            part.readPixels(dw.min.y, dw.max.y);

            if (readColor && readDepth && readMotion && readDiffuse && readSpecAlbedo &&
                readNormals && readRoughness && f.hasSpecularMotion && f.hasBackwardMotion) {
                break; // Blender writes the optional motion passes as later EXR parts.
            }
        }

        // Validate mandatory channels.
        const bool ok = readColor && readDepth && readMotion &&
                        readDiffuse && readSpecAlbedo && readNormals && readRoughness;
        if (!ok) {
            std::string missing;
            if (!readColor)      missing += " Image/RGBA";
            if (!readDepth)      missing += " DenoisingDepth";
            if (!readMotion)     missing += " Vector";
            if (!readDiffuse)    missing += " DenoisingAlbedo";
            if (!readSpecAlbedo) missing += " DenoisingSpecularAlbedo";
            if (!readNormals)    missing += " DenoisingNormal";
            if (!readRoughness)  missing += " DenoisingRoughness";
            return std::unexpected(RrReadError{RrReadErrorCode::MissingMandatoryChannel,
                "Missing mandatory RR channels [" + missing + " ] in: " + path.string(), path});
        }

        LOG_DEBUG("[RrFramePrefetcher] {} {}x{} specMotion={} backMotion={}",
                  path.filename().string(), layout.width, layout.height,
                  f.hasSpecularMotion, f.hasBackwardMotion);
        return f;

    } catch (const std::exception& e) {
        return std::unexpected(RrReadError{RrReadErrorCode::ParseFailed,
            std::string("EXR parse error: ") + e.what(), path});
    } catch (...) {
        return std::unexpected(RrReadError{RrReadErrorCode::ParseFailed,
            "Unknown EXR parse error", path});
    }
}

} // namespace

// =============================================================================
// RrFramePrefetcher
// =============================================================================

RrFramePrefetcher::RrFramePrefetcher(std::vector<std::filesystem::path> framePaths,
                                     int prefetchDepth,
                                     bool waitForFiles,
                                     const std::atomic<bool>* externalStop,
                                     bool atomicInputFiles)
    : m_paths(std::move(framePaths))
    , m_prefetchDepth(std::max(1, prefetchDepth))
    , m_waitForFiles(waitForFiles)
    , m_externalStop(externalStop)
    , m_atomicInputFiles(atomicInputFiles)
{
    static std::once_flag exrOnce;
    std::call_once(exrOnce, [] {
        const int n = std::max(2, static_cast<int>(std::thread::hardware_concurrency()));
        Imf::setGlobalThreadCount(n);
        LOG_INFO("[RrFramePrefetcher] OpenEXR threads={}", n);
    });
    if (m_waitForFiles) {
        LOG_INFO("[RrFramePrefetcher] wait-for-files mode (watch input)");
    }
    m_worker = std::thread([this] { workerLoop(); });
}

RrFramePrefetcher::~RrFramePrefetcher() {
    { std::lock_guard<std::mutex> lk(m_mutex); m_stop.store(true); }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void RrFramePrefetcher::waitUntilWritten(const std::filesystem::path& path) const {
    // Watch mode: Blender streams the EXR to the RAM disk. Wait for existence and
    // a stable size so a partially-written file is never decoded.
    constexpr auto kTick        = std::chrono::milliseconds(100);
    constexpr int  kStableTicks = 3;
    std::uintmax_t lastSize     = 0;
    int            stable       = 0;
    while (!stopRequested()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (m_atomicInputFiles && !ec && size > 0) return;
        if (!ec && size > 0 && size == lastSize) {
            if (++stable >= kStableTicks) return;
        } else {
            stable   = 0;
            lastSize = ec ? 0 : size;
        }
        std::this_thread::sleep_for(kTick);
    }
}

void RrFramePrefetcher::workerLoop() {
    const int total = (int)m_paths.size();
    for (int idx = 0; idx < total; ++idx) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            // Timed wait — same no-notify-on-stop shape as getNext().
            while (!m_cv.wait_for(lk, std::chrono::milliseconds(100), [&] {
                return stopRequested() ||
                       (m_produced.load() - m_consumed.load()) < m_prefetchDepth;
            })) {
            }
            if (stopRequested()) return;
        }

        if (m_waitForFiles) {
            waitUntilWritten(m_paths[idx]);
            if (stopRequested()) return;
        }

        RrFrameResult result = decodeRrExr(m_paths[idx]);

        // Gap detection
        if (result.has_value() && idx + 1 < total) {
            const int thisIdx = extractIndex(m_paths[idx]);
            const int nextIdx = extractIndex(m_paths[idx + 1]);
            if (thisIdx >= 0 && nextIdx >= 0 && (nextIdx - thisIdx) > 1) {
                result->has_gap_after = true;
            }
        }

        { std::lock_guard<std::mutex> lk(m_mutex); m_queue.push(std::move(result)); m_produced.fetch_add(1); }
        m_cv.notify_all();
    }
}

RrFrameResult RrFramePrefetcher::decodeFile(const std::filesystem::path& path) {
    return decodeRrExr(path);
}

RrFrameResult RrFramePrefetcher::getNext() {
    std::unique_lock<std::mutex> lk(m_mutex);
    // Timed wait, not wait(): stop() only sets a flag and nobody notifies this
    // cv. In watch mode the consumer blocks here while the producer is stuck in
    // waitUntilWritten for a file that stops arriving after a job stop — the
    // producer then exits without notifying and a plain wait() slept forever,
    // so the mov trailer was never written (the "stop takes ages to land the
    // mov" report). Re-evaluate the predicate every 100 ms.
    while (!m_cv.wait_for(lk, std::chrono::milliseconds(100),
                          [&] { return !m_queue.empty() || stopRequested(); })) {
    }
    if (m_queue.empty()) {
        return std::unexpected(RrReadError{RrReadErrorCode::IoError,
            "Prefetcher queue empty (stopped or no more frames)", {}});
    }
    RrFrameResult result = std::move(m_queue.front());
    m_queue.pop();
    m_consumed.fetch_add(1);
    lk.unlock();
    m_cv.notify_all();
    return result;
}

} // namespace dlss
