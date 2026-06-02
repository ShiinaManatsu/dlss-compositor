#pragma once
// =============================================================================
// EncoderRegistry — extensible map of OutputFormat → encoder function.
//
// v2 implements ONLY: ExrLinear, Png16, ProRes422HQ.
// All other OutputFormat values (Jpeg, H264Mp4, H265Mp4, Av1Mp4, Avif,
// ProRes4444XQ, DNxHR) are enum-only placeholders for future extensions and
// are NOT registered in globalEncoderRegistry().
// =============================================================================

#include "../core/types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dlss {

struct EncodeParams {
    uint32_t       width        = 0;
    uint32_t       height       = 0;
    const uint8_t* pixels       = nullptr;  // RGBA half for EXR / RGBA u16 for PNG / RGBA u8 for ProRes
    std::size_t    pixelByteSize = 0;
    int            fps          = 24;       // for video encoders
    bool           is_hdr       = false;
};

/// Encoder function: takes params + output path, writes file or appends to video.
/// Throws std::runtime_error (or similar) on failure — AsyncFrameWriter records
/// these as WriteError entries.
using EncoderFn = std::function<void(const EncodeParams&, const std::filesystem::path&)>;

/// Registry of encoder functions. Extensible for future codecs.
/// v2 implements ONLY: ExrLinear, Png16, ProRes422HQ.
class EncoderRegistry {
public:
    void registerEncoder(OutputFormat fmt, EncoderFn fn);
    bool hasEncoder(OutputFormat fmt) const;
    EncoderFn getEncoder(OutputFormat fmt) const;

    /// Returns list of registered formats (order undefined).
    std::vector<OutputFormat> registeredFormats() const;

private:
    std::unordered_map<int, EncoderFn> m_encoders;
};

/// Get the global encoder registry (initialized lazily with v2 encoders).
/// Contains ONLY: ExrLinear, Png16, ProRes422HQ.
/// OutputFormat::Jpeg / H264Mp4 / H265Mp4 / Av1Mp4 / Avif / ProRes4444XQ /
/// DNxHR are enum placeholders — NOT registered.
EncoderRegistry& globalEncoderRegistry();

} // namespace dlss
