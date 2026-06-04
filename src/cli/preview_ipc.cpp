#include "preview_ipc.h"

#include "../core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dlss {

namespace {

constexpr std::uint32_t kPreviewMagic = 0x44505350u; // "PSPD" little-endian marker
constexpr std::uint32_t kPreviewVersion = 1;
constexpr std::uint32_t kPreviewPixelBytes = 8u * 1024u * 1024u;
constexpr std::uint32_t kPreviewMaxPixels = kPreviewPixelBytes / 4u;

struct PreviewHeader {
    std::uint32_t magic = kPreviewMagic;
    std::uint32_t version = kPreviewVersion;
    std::uint32_t generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t byteSize = 0;
    std::uint32_t reserved = 0;
};

float halfToFloat(std::uint16_t h) noexcept {
    const std::uint32_t sign = (h >> 15) & 0x1u;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;

    if (exponent == 0) {
        if (mantissa == 0) {
            return sign ? -0.0f : 0.0f;
        }
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -value : value;
    }
    if (exponent == 31) {
        return mantissa == 0
            ? (sign ? -INFINITY : INFINITY)
            : NAN;
    }
    const float value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                                  static_cast<int>(exponent) - 15);
    return sign ? -value : value;
}

std::uint8_t toByte(float v) noexcept {
    if (!std::isfinite(v)) {
        return 0;
    }
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

std::uint16_t readLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

} // namespace

PreviewPublisher::PreviewPublisher(std::string sharedMemoryName)
    : m_sharedMemoryName(std::move(sharedMemoryName)) {}

PreviewPublisher::~PreviewPublisher() {
#ifdef _WIN32
    closeMapping();
#endif
}

bool PreviewPublisher::enabled() const noexcept {
    return !m_sharedMemoryName.empty();
}

void PreviewPublisher::publishRgba16f(const std::vector<std::uint8_t>& rgba16f,
                                      std::uint32_t width,
                                      std::uint32_t height) {
    if (!enabled() || rgba16f.empty() || width == 0 || height == 0) {
        return;
    }

#ifdef _WIN32
    ensureMapping();
    if (!m_view) {
        return;
    }

    const std::uint64_t srcPixels = static_cast<std::uint64_t>(width) * height;
    if (srcPixels == 0 || rgba16f.size() < srcPixels * 8ull) {
        return;
    }

    std::uint32_t outW = width;
    std::uint32_t outH = height;
    if (srcPixels > kPreviewMaxPixels) {
        const double scale = std::sqrt(static_cast<double>(kPreviewMaxPixels) /
                                       static_cast<double>(srcPixels));
        outW = std::max(1u, static_cast<std::uint32_t>(std::floor(width * scale)));
        outH = std::max(1u, static_cast<std::uint32_t>(std::floor(height * scale)));
    }

    PreviewHeader header;
    header.generation = ++m_generation;
    header.width = outW;
    header.height = outH;
    header.byteSize = outW * outH * 4u;

    auto* base = static_cast<std::uint8_t*>(m_view);
    std::memcpy(base, &header, sizeof(header));
    auto* dst = base + sizeof(PreviewHeader);

    for (std::uint32_t y = 0; y < outH; ++y) {
        const std::uint32_t srcY = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * height) / outH);
        for (std::uint32_t x = 0; x < outW; ++x) {
            const std::uint32_t srcX = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * width) / outW);
            const std::uint64_t srcOffset =
                (static_cast<std::uint64_t>(srcY) * width + srcX) * 8ull;
            const std::uint64_t dstOffset =
                (static_cast<std::uint64_t>(y) * outW + x) * 4ull;

            dst[dstOffset + 0] = toByte(halfToFloat(readLe16(&rgba16f[static_cast<std::size_t>(srcOffset + 0)])));
            dst[dstOffset + 1] = toByte(halfToFloat(readLe16(&rgba16f[static_cast<std::size_t>(srcOffset + 2)])));
            dst[dstOffset + 2] = toByte(halfToFloat(readLe16(&rgba16f[static_cast<std::size_t>(srcOffset + 4)])));
            dst[dstOffset + 3] = toByte(halfToFloat(readLe16(&rgba16f[static_cast<std::size_t>(srcOffset + 6)])));
        }
    }
#else
    (void)rgba16f;
    (void)width;
    (void)height;
#endif
}

#ifdef _WIN32
void PreviewPublisher::ensureMapping() {
    if (m_view) {
        return;
    }

    const DWORD totalSize = static_cast<DWORD>(sizeof(PreviewHeader) + kPreviewPixelBytes);
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                        nullptr,
                                        PAGE_READWRITE,
                                        0,
                                        totalSize,
                                        m_sharedMemoryName.c_str());
    if (!mapping) {
        LOG_WARN("PreviewPublisher: CreateFileMapping failed for {}", m_sharedMemoryName);
        return;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, totalSize);
    if (!view) {
        LOG_WARN("PreviewPublisher: MapViewOfFile failed for {}", m_sharedMemoryName);
        CloseHandle(mapping);
        return;
    }

    m_mapping = mapping;
    m_view = view;
}

void PreviewPublisher::closeMapping() noexcept {
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }
}
#endif

} // namespace dlss
