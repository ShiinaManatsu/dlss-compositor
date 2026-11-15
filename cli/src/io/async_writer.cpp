// =============================================================================
// AsyncFrameWriter implementation + globalEncoderRegistry.
//
// v2 registers ONLY: ExrLinear and Png16, both via OpenImageIO.
// Other OutputFormat values are enum placeholders and intentionally NOT
// registered.
// =============================================================================

#include "async_writer.h"

#include "../core/log.h"
#include "../core/path_encoding.h"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace dlss {

// =============================================================================
// EncoderRegistry
// =============================================================================
void EncoderRegistry::registerEncoder(OutputFormat fmt, EncoderFn fn)
{
    m_encoders[static_cast<int>(fmt)] = std::move(fn);
}

bool EncoderRegistry::hasEncoder(OutputFormat fmt) const
{
    return m_encoders.find(static_cast<int>(fmt)) != m_encoders.end();
}

EncoderFn EncoderRegistry::getEncoder(OutputFormat fmt) const
{
    auto it = m_encoders.find(static_cast<int>(fmt));
    if (it == m_encoders.end()) {
        return EncoderFn{};
    }
    return it->second;
}

std::vector<OutputFormat> EncoderRegistry::registeredFormats() const
{
    std::vector<OutputFormat> out;
    out.reserve(m_encoders.size());
    for (const auto& kv : m_encoders) {
        out.push_back(static_cast<OutputFormat>(kv.first));
    }
    return out;
}

// =============================================================================
// Built-in encoders (v2)
// =============================================================================
namespace {

int percentToZlibLevel(int percent) noexcept
{
    const int clamped = std::clamp(percent, 0, 100);
    return static_cast<int>(std::lround(static_cast<double>(clamped) * 9.0 / 100.0));
}

const char* oiioExrCompression(ExrCompressionMode mode) noexcept
{
    switch (mode) {
    case ExrCompressionMode::None: return "none";
    case ExrCompressionMode::Dwaa: return "dwaa";
    case ExrCompressionMode::Zip:  return "zip";
    }
    return "zip";
}

// ---------- EXR (linear, 16-bit half RGBA) via OpenImageIO -----------------
void encodeExrLinear(const EncodeParams& p, const std::filesystem::path& outputPath)
{
    if (p.pixels == nullptr || p.width == 0 || p.height == 0) {
        throw std::runtime_error("encodeExrLinear: empty pixel buffer or zero dimensions");
    }
    const std::size_t expected_bytes =
        static_cast<std::size_t>(p.width) *
        static_cast<std::size_t>(p.height) * 4u * sizeof(std::uint16_t);
    if (p.pixelByteSize != 0 && p.pixelByteSize < expected_bytes) {
        throw std::runtime_error("encodeExrLinear: pixel buffer too small for declared dimensions");
    }

    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    OIIO::ImageSpec spec(
        static_cast<int>(p.width),
        static_cast<int>(p.height),
        4,
        OIIO::TypeDesc::HALF);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.attribute("compression", oiioExrCompression(p.options.exrCompression));
    spec.attribute("openexr:zipCompressionLevel",
                   percentToZlibLevel(p.options.exrCompressionLevelPercent));
    spec.attribute("openexr:dwaCompressionLevel",
                   static_cast<float>(std::clamp(p.options.exrCompressionLevelPercent, 0, 100)));
    spec.attribute("oiio:ColorSpace", "lin_rec709");

    auto out = OIIO::ImageOutput::create(pathToUtf8(outputPath));
    if (!out) {
        throw std::runtime_error("encodeExrLinear: cannot create OIIO output: " + pathToUtf8(outputPath));
    }
    if (!out->open(pathToUtf8(outputPath), spec)) {
        throw std::runtime_error("encodeExrLinear: cannot open output: " + pathToUtf8(outputPath)
                                 + ": " + out->geterror());
    }
    if (!out->write_image(OIIO::TypeDesc::HALF, p.pixels)) {
        const std::string err = out->geterror();
        out->close();
        throw std::runtime_error("encodeExrLinear: write failed: " + pathToUtf8(outputPath) + ": " + err);
    }
    if (!out->close()) {
        throw std::runtime_error("encodeExrLinear: close failed: " + pathToUtf8(outputPath)
                                 + ": " + out->geterror());
    }
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

std::uint16_t toPng16(float value, bool encodeSrgb) noexcept
{
    const float encoded = encodeSrgb ? srgbEncode(value) : std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::clamp(encoded, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

// ---------- PNG16 ------------------------------------------------------------
void encodePng16(const EncodeParams& p, const std::filesystem::path& outputPath)
{
    if (p.pixels == nullptr || p.width == 0 || p.height == 0) {
        throw std::runtime_error("encodePng16: empty pixel buffer or zero dimensions");
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height) * 4u * sizeof(std::uint16_t);
    if (p.pixelByteSize < expectedBytes) {
        throw std::runtime_error("encodePng16: pixel buffer too small for declared dimensions");
    }

    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    std::vector<std::uint16_t> rgba16(static_cast<std::size_t>(p.width) * p.height * 4u);
    for (std::uint32_t y = 0; y < p.height; ++y) {
        for (std::uint32_t x = 0; x < p.width; ++x) {
            const std::size_t pixelIndex = (static_cast<std::size_t>(y) * p.width + x) * 4u;
            for (std::uint32_t ch = 0; ch < 4; ++ch) {
                const auto half = readLe16(p.pixels + (pixelIndex + ch) * sizeof(std::uint16_t));
                const bool encodeSrgb = ch < 3 && p.pixelEncoding == PixelEncoding::SceneLinear;
                rgba16[pixelIndex + ch] = toPng16(halfToFloat(half), encodeSrgb);
            }
        }
    }

    OIIO::ImageSpec spec(
        static_cast<int>(p.width),
        static_cast<int>(p.height),
        4,
        OIIO::TypeDesc::UINT16);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.attribute("png:compressionLevel", percentToZlibLevel(p.options.pngQualityPercent));

    auto out = OIIO::ImageOutput::create(pathToUtf8(outputPath));
    if (!out) {
        throw std::runtime_error("encodePng16: cannot create OIIO output: " + pathToUtf8(outputPath));
    }
    if (!out->open(pathToUtf8(outputPath), spec)) {
        const std::string err = out->geterror();
        throw std::runtime_error("encodePng16: cannot open output: " + pathToUtf8(outputPath) + ": " + err);
    }
    if (!out->write_image(OIIO::TypeDesc::UINT16, rgba16.data())) {
        const std::string err = out->geterror();
        out->close();
        throw std::runtime_error("encodePng16: write failed: " + pathToUtf8(outputPath) + ": " + err);
    }
    if (!out->close()) {
        throw std::runtime_error("encodePng16: close failed: " + pathToUtf8(outputPath) + ": " + out->geterror());
    }
}

} // namespace

EncoderRegistry& globalEncoderRegistry()
{
    static EncoderRegistry s_registry = [] {
        EncoderRegistry r;
        r.registerEncoder(OutputFormat::ExrLinear,   &encodeExrLinear);
        r.registerEncoder(OutputFormat::Png16,       &encodePng16);
        // ProRes422HQ is handled by ProResWriter at pipeline level because it
        // owns a single MOV muxer across the whole sequence, not per-frame jobs.
        // INTENTIONAL: Jpeg/H264Mp4/H265Mp4/Av1Mp4/Avif/ProRes4444XQ/DNxHR are
        // enum-only placeholders for future extensions and are NOT registered.
        return r;
    }();
    return s_registry;
}

std::vector<uint8_t> encodePreviewJpeg(uint32_t width, uint32_t height,
                                       const std::vector<uint8_t>& rgba8,
                                       int quality)
{
    if (width == 0 || height == 0) {
        return {};
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (rgba8.size() < pixels * 4u) {
        return {};
    }

    std::vector<unsigned char> rgb(pixels * 3u);
    for (std::size_t i = 0; i < pixels; ++i) {
        rgb[i * 3u + 0] = rgba8[i * 4u + 0];
        rgb[i * 3u + 1] = rgba8[i * 4u + 1];
        rgb[i * 3u + 2] = rgba8[i * 4u + 2];
    }

    std::vector<unsigned char> jpeg;
    OIIO::Filesystem::IOVecOutput proxy(jpeg);
    auto out = OIIO::ImageOutput::create("jpg");
    if (!out) {
        LOG_ERROR("encodePreviewJpeg: OIIO has no JPEG plugin");
        return {};
    }
    OIIO::ImageSpec spec(static_cast<int>(width), static_cast<int>(height), 3, OIIO::TypeDesc::UINT8);
    spec.attribute("CompressionQuality", std::clamp(quality, 1, 100));
    if (!out->set_ioproxy(&proxy) || !out->open("preview.jpg", spec)) {
        LOG_ERROR("encodePreviewJpeg: open failed: {}", out->geterror());
        return {};
    }
    if (!out->write_image(OIIO::TypeDesc::UINT8, rgb.data())) {
        LOG_ERROR("encodePreviewJpeg: write failed: {}", out->geterror());
        out->close();
        return {};
    }
    if (!out->close()) {
        LOG_ERROR("encodePreviewJpeg: close failed: {}", out->geterror());
        return {};
    }
    return {jpeg.begin(), jpeg.end()};
}

// =============================================================================
// Helpers
// =============================================================================
namespace {

int resolveThreadCount(int ctorValue)
{
    if (ctorValue > 0) {
        return ctorValue;
    }
    if (const char* env = std::getenv("DLSS_COMPOSITOR_WRITER_THREADS")) {
        try {
            int v = std::stoi(env);
            if (v > 0) {
                return v;
            }
        } catch (...) { /* fall through */ }
    }
    return 4;
}

int resolveHighWatermark(int explicitValue, int threadCount)
{
    if (explicitValue > 0) {
        return explicitValue;
    }
    return std::max(threadCount * 3, threadCount);
}

int resolveLowWatermark(int explicitValue, int highWatermark, int threadCount)
{
    if (explicitValue > 0) {
        return std::clamp(explicitValue, 0, highWatermark);
    }
    return std::clamp(threadCount, 0, highWatermark);
}

std::string zeroPad(int value, int width)
{
    std::ostringstream os;
    os.setf(std::ios::internal, std::ios::adjustfield);
    os.width(width);
    os.fill('0');
    os << value;
    return os.str();
}

std::filesystem::path insertVersionSuffix(const std::filesystem::path& path, int version)
{
    if (version <= 0) {
        return path;
    }
    const std::filesystem::path parent = path.parent_path();
    const std::string           stem   = pathToUtf8(path.stem());
    const std::string           ext    = pathToUtf8(path.extension());
    const std::string suffix = "_v" + zeroPad(version, 2);
    return parent / pathFromUtf8(stem + suffix + ext);
}

} // namespace

// =============================================================================
// AsyncFrameWriter
// =============================================================================

AsyncFrameWriter::AsyncFrameWriter(int numThreads, int queueHighWatermark, int queueLowWatermark)
    : m_numThreads(resolveThreadCount(numThreads)),
      m_queueHighWatermark(resolveHighWatermark(queueHighWatermark, m_numThreads)),
      m_queueLowWatermark(resolveLowWatermark(queueLowWatermark, m_queueHighWatermark, m_numThreads))
{
    m_workers.reserve(static_cast<std::size_t>(m_numThreads));
    for (int i = 0; i < m_numThreads; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
    LOG_INFO("AsyncFrameWriter started with {} worker thread(s), queue low/high={}/{}",
             m_numThreads,
             m_queueLowWatermark,
             m_queueHighWatermark);
}

AsyncFrameWriter::~AsyncFrameWriter()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void AsyncFrameWriter::enqueue(WriteJob job)
{
    if (job.previewCallback) {
        job.previewCallback(job.pixels, job.width, job.height);
    }
    if (job.format == OutputFormat::Png16 && job.is_hdr) {
        // Per T29 plan: PNG cannot represent HDR — reject at enqueue.
        throw std::runtime_error(
            "AsyncFrameWriter::enqueue: HDR + PNG is unsupported "
            "(use ExrLinear or ProRes422HQ for HDR output)");
    }
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_pending.load(std::memory_order_acquire) >= m_queueHighWatermark) {
            m_capacityCv.wait(lock, [&] {
                return m_stop.load() ||
                       m_pending.load(std::memory_order_acquire) <= m_queueLowWatermark;
            });
        }
        if (m_stop.load()) {
            throw std::runtime_error("AsyncFrameWriter::enqueue after cancel/stop");
        }
        m_queue.push(std::move(job));
        m_pending.fetch_add(1, std::memory_order_acq_rel);
    }
    m_cv.notify_one();
}

void AsyncFrameWriter::flush()
{
    std::unique_lock<std::mutex> lock(m_flushMutex);
    m_flushCv.wait(lock, [&] {
        return m_pending.load(std::memory_order_acquire) == 0;
    });
}

void AsyncFrameWriter::cancel()
{
    int discarded = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
        while (!m_queue.empty()) {
            m_queue.pop();
            ++discarded;
        }
    }
    if (discarded > 0) {
        m_pending.fetch_sub(discarded, std::memory_order_acq_rel);
    }
    m_cv.notify_all();
    m_capacityCv.notify_all();
    std::lock_guard<std::mutex> flock(m_flushMutex);
    m_flushCv.notify_all();
}

bool AsyncFrameWriter::hasErrors() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return !m_errors.empty();
}

std::vector<WriteError> AsyncFrameWriter::getErrors() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_errors;
}

void AsyncFrameWriter::recordError(WriteError err)
{
    LOG_ERROR("AsyncFrameWriter: {} ({})", err.message, err.path.string());
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_errors.push_back(std::move(err));
}

void AsyncFrameWriter::notifyCapacityAvailable()
{
    if (m_pending.load(std::memory_order_acquire) <= m_queueLowWatermark) {
        m_capacityCv.notify_all();
    }
}

void AsyncFrameWriter::workerLoop()
{
    for (;;) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop.load() && m_queue.empty()) {
                return;
            }
            if (m_queue.empty()) {
                continue;
            }
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        try {
            processJob(job);
            if (job.completionCallback) {
                job.completionCallback();
            }
        } catch (const std::exception& e) {
            recordError(WriteError{
                WriteErrorCode::EncoderFailed,
                std::string("Encoder threw: ") + e.what(),
                job.outputPath});
        } catch (...) {
            recordError(WriteError{
                WriteErrorCode::EncoderFailed,
                "Encoder threw unknown exception",
                job.outputPath});
        }

        // Decrement pending and wake flush() if drained.
        const int remaining = m_pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
        notifyCapacityAvailable();
        if (remaining == 0) {
            std::lock_guard<std::mutex> flock(m_flushMutex);
            m_flushCv.notify_all();
        }
    }
}

std::filesystem::path AsyncFrameWriter::resolveOutputPath(const WriteJob& job,
                                                          bool&            outShouldSkip)
{
    outShouldSkip = false;
    std::error_code ec;
    const bool exists = std::filesystem::exists(job.outputPath, ec);

    switch (job.overwriteMode) {
    case OverwriteMode::Overwrite:
        return job.outputPath;
    case OverwriteMode::Skip:
        if (exists) {
            LOG_WARN("AsyncFrameWriter: skipping existing file {}", pathToUtf8(job.outputPath));
            outShouldSkip = true;
        }
        return job.outputPath;
    case OverwriteMode::AutoVersion: {
        if (!exists) {
            return job.outputPath;
        }
        for (int v = 1; v < 1000; ++v) {
            std::filesystem::path candidate = insertVersionSuffix(job.outputPath, v);
            if (!std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
        // Pathological — fall back to overwrite to avoid infinite loop.
        return job.outputPath;
    }
    }
    return job.outputPath;
}

void AsyncFrameWriter::processJob(WriteJob& job)
{
    bool shouldSkip = false;
    const std::filesystem::path resolved = resolveOutputPath(job, shouldSkip);
    if (shouldSkip) {
        return;
    }

    const auto& reg = globalEncoderRegistry();
    if (!reg.hasEncoder(job.format)) {
        throw std::runtime_error("No encoder registered for OutputFormat=" +
                                 std::to_string(static_cast<int>(job.format)));
    }
    EncoderFn enc = reg.getEncoder(job.format);

    EncodeParams params;
    params.width         = job.width;
    params.height        = job.height;
    params.pixels        = job.pixels.data();
    params.pixelByteSize = job.pixels.size();
    params.fps           = job.fps;
    params.is_hdr        = job.is_hdr;
    params.pixelEncoding = job.pixelEncoding;
    params.options       = job.encoderOptions;

    enc(params, resolved);
}

// =============================================================================
// Static helpers — formatPath + checkOutputCollisions
// =============================================================================

std::string AsyncFrameWriter::formatPath(const std::string& tmpl, int frameNum, int version)
{
    std::string out = tmpl;

    // 1. Replace runs of '#' with zero-padded frame number of matching width.
    {
        std::string::size_type i = 0;
        while (i < out.size()) {
            if (out[i] == '#') {
                std::string::size_type j = i;
                while (j < out.size() && out[j] == '#') {
                    ++j;
                }
                const int width = static_cast<int>(j - i);
                std::string padded = zeroPad(frameNum, width);
                out.replace(i, j - i, padded);
                i += padded.size();
            } else {
                ++i;
            }
        }
    }

    // 2. Replace {frame:0Nd} / {frame} placeholders.
    {
        static const std::regex re(R"(\{frame(?::0?(\d+)d)?\})");
        std::smatch m;
        std::string result;
        std::string::const_iterator search_start = out.cbegin();
        while (std::regex_search(search_start, out.cend(), m, re)) {
            int width = 1;
            if (m[1].matched) {
                try { width = std::stoi(m[1].str()); }
                catch (...) { width = 1; }
            }
            result.append(search_start, m[0].first);
            result.append(zeroPad(frameNum, width));
            search_start = m[0].second;
        }
        result.append(search_start, out.cend());
        out = std::move(result);
    }

    // 3. Apply optional version suffix.
    if (version > 0) {
        const std::filesystem::path p = pathFromUtf8(out);
        out = pathToUtf8(insertVersionSuffix(p, version));
    }
    return out;
}

int AsyncFrameWriter::checkOutputCollisions(const std::filesystem::path& outputDir,
                                            const std::string&           filenameTemplate,
                                            int                          frameCount,
                                            OutputFormat                 /*fmt*/)
{
    int collisions = 0;
    std::error_code ec;
    for (int frame = 0; frame < frameCount; ++frame) {
        const std::string       name = formatPath(filenameTemplate, frame);
        const std::filesystem::path p = outputDir / pathFromUtf8(name);
        if (std::filesystem::exists(p, ec)) {
            ++collisions;
        }
    }
    return collisions;
}

} // namespace dlss
