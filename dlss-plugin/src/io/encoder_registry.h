#pragma once
// =============================================================================
// EncoderRegistry — extensible map of OutputFormat → encoder function.
//
// v2 image sequence encoders implement ONLY: ExrLinear and Png16.
// ProRes422HQ is a sequence-level MOV muxer handled by ProResWriter, not this
// per-frame registry. Other OutputFormat values remain enum-only placeholders.
// =============================================================================

#include "../core/types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dlss {

enum class ExrCompressionMode {
    None,
    Zip,
    Dwaa,
};

struct EncoderOptions {
    int pngQualityPercent = 97;
    ExrCompressionMode exrCompression = ExrCompressionMode::Zip;
    int exrCompressionLevelPercent = 97;
};

enum class PixelEncoding {
    SceneLinear,
    DisplayReady,
};

struct EncodeParams {
    uint32_t       width        = 0;
    uint32_t       height       = 0;
    const uint8_t* pixels       = nullptr;  // RGBA half for EXR / RGBA u16 for PNG / RGBA u8 for ProRes
    std::size_t    pixelByteSize = 0;
    int            fps          = 24;       // for video encoders
    bool           is_hdr       = false;
    PixelEncoding  pixelEncoding = PixelEncoding::SceneLinear;
    EncoderOptions options{};
};

/// Encoder function: takes params + output path, writes file or appends to video.
/// Throws std::runtime_error (or similar) on failure — AsyncFrameWriter records
/// these as WriteError entries.
using EncoderFn = std::function<void(const EncodeParams&, const std::filesystem::path&)>;

/// Registry of encoder functions. Extensible for future codecs.
/// v2 image sequence encoders implement ONLY: ExrLinear and Png16.
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
/// Contains ONLY: ExrLinear and Png16. ProRes422HQ is sequence-level and is not
/// registered here.
EncoderRegistry& globalEncoderRegistry();

} // namespace dlss
