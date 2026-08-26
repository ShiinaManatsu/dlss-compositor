// RrFramePrefetcher implementation.
// Reads Blender 5.3 RR multilayer EXR files; decodes all guide buffer channels.

#include "rr_frame_prefetcher.h"
#include "../core/log.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace dlss {

namespace {

constexpr int kMaxDim    = 16384;
constexpr std::size_t kMaxPixels = 16384ull * 16384ull;

// ---------------------------------------------------------------------------
// Blender 5.3 RR channel names (inject.py RR_FILE_ITEMS + core buffers).
// ---------------------------------------------------------------------------
// Combined
static constexpr const char* kColorChannels[4]    = {"Image.R","Image.G","Image.B","Image.A"};
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
        Imf::MultiPartInputFile multi(path.string().c_str());
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

            if (!readColor && hasCh(channels, "Image.R")) {
                insertInterleavedSlices(fb, f.colorRgba.data(), kColorChannels, 4, channels, layout, dw);
                readColor = true;
            }
            if (!readDepth && hasCh(channels, kDepthChannel)) {
                insertScalarSlice(fb, f.depthLinear.data(), kDepthChannel, channels, layout, dw);
                readDepth = true;
            }
            if (!readMotion && hasCh(channels, "Vector.X")) {
                insertInterleavedSlices(fb, f.motionXy.data(), kMotionChannels, 2, channels, layout, dw);
                readMotion = true;
            }
            if (!readDiffuse && hasCh(channels, "Denoising Albedo.R")) {
                insertInterleavedSlices(fb, f.diffuseAlbedoRgba.data(), kDiffChannels, 4, channels, layout, dw);
                readDiffuse = true;
            }
            if (!readSpecAlbedo && hasCh(channels, "Denoising Specular Albedo.R")) {
                insertInterleavedSlices(fb, f.specAlbedoRgba.data(), kSpecChannels, 4, channels, layout, dw);
                readSpecAlbedo = true;
            }
            if (!readNormals && hasCh(channels, "Denoising Normal.X")) {
                insertInterleavedSlices(fb, f.normalsXyz.data(), kNormChannels, 3, channels, layout, dw);
                readNormals = true;
            }
            if (!readRoughness && hasCh(channels, kRoughChannel)) {
                insertScalarSlice(fb, f.roughness.data(), kRoughChannel, channels, layout, dw);
                readRoughness = true;
            }
            if (!f.hasSpecularMotion && hasCh(channels, "Denoising Specular Motion.X")) {
                insertInterleavedSlices(fb, f.specularMotionXy.data(), kSpecMotionChannels, 2, channels, layout, dw);
                f.hasSpecularMotion = true;
            }
            if (!f.hasBackwardMotion && hasCh(channels, "Denoising Backward Motion.X")) {
                insertInterleavedSlices(fb, f.backwardMotionXy.data(), kBackMotionChannels, 2, channels, layout, dw);
                f.hasBackwardMotion = true;
            }

            // Only read this part if we inserted at least one slice.
            // Check by seeing if fb is non-empty (Imf::FrameBuffer doesn't expose size, use flags).
            bool anySlice = readColor || readDepth || readMotion || readDiffuse ||
                            readSpecAlbedo || readNormals || readRoughness ||
                            f.hasSpecularMotion || f.hasBackwardMotion;
            (void)anySlice;
            // Always call setFrameBuffer + readPixels for parts where we inserted slices.
            // Safe: inserting into an empty FrameBuffer is a no-op read.
            part.setFrameBuffer(fb);
            part.readPixels(dw.min.y, dw.max.y);

            if (readColor && readDepth && readMotion &&
                readDiffuse && readSpecAlbedo && readNormals && readRoughness) {
                break; // All mandatory channels found.
            }
        }

        // Validate mandatory channels.
        const bool ok = readColor && readDepth && readMotion &&
                        readDiffuse && readSpecAlbedo && readNormals && readRoughness;
        if (!ok) {
            std::string missing;
            if (!readColor)      missing += " Image";
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
                                     int prefetchDepth)
    : m_paths(std::move(framePaths))
    , m_prefetchDepth(std::max(1, prefetchDepth))
{
    m_worker = std::thread([this] { workerLoop(); });
}

RrFramePrefetcher::~RrFramePrefetcher() {
    { std::lock_guard<std::mutex> lk(m_mutex); m_stop.store(true); }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void RrFramePrefetcher::workerLoop() {
    const int total = (int)m_paths.size();
    for (int idx = 0; idx < total; ++idx) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [&] {
                return m_stop.load() ||
                       (m_produced.load() - m_consumed.load()) < m_prefetchDepth;
            });
            if (m_stop.load()) return;
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

RrFrameResult RrFramePrefetcher::getNext() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cv.wait(lk, [&] { return !m_queue.empty() || m_stop.load(); });
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
