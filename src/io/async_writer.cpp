// =============================================================================
// AsyncFrameWriter implementation + globalEncoderRegistry.
//
// v2 registers ONLY: ExrLinear and Png16.
// Other OutputFormat values are enum placeholders and intentionally NOT
// registered.
// =============================================================================

#include "async_writer.h"

#include "../core/log.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPixelType.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
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

Imf::Compression toOpenExrCompression(ExrCompressionMode mode) noexcept
{
    switch (mode) {
    case ExrCompressionMode::None: return Imf::NO_COMPRESSION;
    case ExrCompressionMode::Dwaa: return Imf::DWAA_COMPRESSION;
    case ExrCompressionMode::Zip:  return Imf::ZIP_COMPRESSION;
    }
    return Imf::ZIP_COMPRESSION;
}

// ---------- EXR (linear, 16-bit half RGBA) ----------------------------------
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

    Imf::Header header(static_cast<int>(p.width), static_cast<int>(p.height));
    header.compression() = toOpenExrCompression(p.options.exrCompression);
    header.zipCompressionLevel() = percentToZlibLevel(p.options.exrCompressionLevelPercent);
    header.dwaCompressionLevel() = static_cast<float>(std::clamp(p.options.exrCompressionLevelPercent, 0, 100));
    header.channels().insert("R", Imf::Channel(Imf::HALF));
    header.channels().insert("G", Imf::Channel(Imf::HALF));
    header.channels().insert("B", Imf::Channel(Imf::HALF));
    header.channels().insert("A", Imf::Channel(Imf::HALF));

    Imf::OutputFile file(outputPath.string().c_str(), header);

    const std::size_t pixel_stride = 4u * sizeof(std::uint16_t);
    const std::size_t row_stride   = pixel_stride * static_cast<std::size_t>(p.width);

    // OpenEXR's Slice takes a non-const char*, but writePixels only reads.
    char* base = const_cast<char*>(reinterpret_cast<const char*>(p.pixels));

    Imf::FrameBuffer fb;
    const char* const names[4] = {"R", "G", "B", "A"};
    for (int ch = 0; ch < 4; ++ch) {
        fb.insert(names[ch],
                  Imf::Slice(Imf::HALF,
                             base + ch * static_cast<std::ptrdiff_t>(sizeof(std::uint16_t)),
                             pixel_stride,
                             row_stride));
    }
    file.setFrameBuffer(fb);
    file.writePixels(static_cast<int>(p.height));
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

void appendU32BE(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
}

void appendChunk(std::vector<std::uint8_t>& png, const char type[4], const std::vector<std::uint8_t>& payload)
{
    appendU32BE(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, png.data() + typeOffset, static_cast<uInt>(png.size() - typeOffset));
    appendU32BE(png, static_cast<std::uint32_t>(crc));
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

    const std::size_t rowBytes = static_cast<std::size_t>(p.width) * 4u * sizeof(std::uint16_t);
    if (rowBytes > std::numeric_limits<std::size_t>::max() - 1u) {
        throw std::runtime_error("encodePng16: row too wide");
    }
    std::vector<std::uint8_t> scanlines((rowBytes + 1u) * static_cast<std::size_t>(p.height));
    for (std::uint32_t y = 0; y < p.height; ++y) {
        std::uint8_t* row = scanlines.data() + static_cast<std::size_t>(y) * (rowBytes + 1u);
        row[0] = 0; // PNG filter type: None.
        for (std::uint32_t x = 0; x < p.width; ++x) {
            const std::size_t pixelIndex = (static_cast<std::size_t>(y) * p.width + x) * 4u;
            for (std::uint32_t ch = 0; ch < 4; ++ch) {
                const auto half = readLe16(p.pixels + (pixelIndex + ch) * sizeof(std::uint16_t));
                const bool encodeSrgb = ch < 3;
                const std::uint16_t value = toPng16(halfToFloat(half), encodeSrgb);
                const std::size_t dst = 1u + (static_cast<std::size_t>(x) * 4u + ch) * sizeof(std::uint16_t);
                row[dst + 0] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
                row[dst + 1] = static_cast<std::uint8_t>(value & 0xffu);
            }
        }
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(compressedSize));
    const int zlibResult = compress2(compressed.data(), &compressedSize,
                                     scanlines.data(), static_cast<uLong>(scanlines.size()),
                                     percentToZlibLevel(p.options.pngQualityPercent));
    if (zlibResult != Z_OK) {
        throw std::runtime_error("encodePng16: zlib compression failed: " + std::to_string(zlibResult));
    }
    compressed.resize(static_cast<std::size_t>(compressedSize));

    std::vector<std::uint8_t> png;
    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), signature, signature + sizeof(signature));

    std::vector<std::uint8_t> ihdr;
    ihdr.reserve(13);
    appendU32BE(ihdr, p.width);
    appendU32BE(ihdr, p.height);
    ihdr.push_back(16); // bit depth
    ihdr.push_back(6);  // RGBA
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", compressed);
    appendChunk(png, "IEND", {});

    std::ofstream f(outputPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error("encodePng16: cannot open output: " + outputPath.string());
    }
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
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

std::string zeroPad(int value, int width)
{
    std::ostringstream os;
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
    const std::string           stem   = path.stem().string();
    const std::string           ext    = path.extension().string();
    const std::string suffix = "_v" + zeroPad(version, 2);
    return parent / (stem + suffix + ext);
}

} // namespace

// =============================================================================
// AsyncFrameWriter
// =============================================================================

AsyncFrameWriter::AsyncFrameWriter(int numThreads)
    : m_numThreads(resolveThreadCount(numThreads))
{
    m_workers.reserve(static_cast<std::size_t>(m_numThreads));
    for (int i = 0; i < m_numThreads; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
    LOG_INFO("AsyncFrameWriter started with {} worker thread(s)", m_numThreads);
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
        std::lock_guard<std::mutex> lock(m_mutex);
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

void AsyncFrameWriter::workerLoop()
{
    for (;;) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop.load() || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_stop.load()) {
                    return;
                }
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
            LOG_WARN("AsyncFrameWriter: skipping existing file {}", job.outputPath.string());
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
        const std::filesystem::path p(out);
        out = insertVersionSuffix(p, version).string();
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
        const std::filesystem::path p = outputDir / name;
        if (std::filesystem::exists(p, ec)) {
            ++collisions;
        }
    }
    return collisions;
}

} // namespace dlss
