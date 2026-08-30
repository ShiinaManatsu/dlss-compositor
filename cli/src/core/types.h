#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace dlss {

struct Resolution { uint32_t width; uint32_t height; };

enum class QualityMode {
    DLAA = 0,
    UltraQuality = 1,
    Quality = 2,
    Balanced = 3,
    Performance = 4,
    UltraPerformance = 5
};

enum class DlssPreset { Auto, J, K, L, M };

struct JitterPair { float x; float y; }; // pixel space [-0.5, +0.5]

struct CameraData {
    float matrix_world[4][4];
    float projection[4][4];
    float fov;
    float aspect_ratio;
    float near_clip;
    float far_clip;
    JitterPair jitter;
};

struct DlssConfig {
    int version = 1;
    Resolution output_resolution{};
    Resolution render_resolution{};
    QualityMode quality{QualityMode::Quality};
    std::string display_device;
    std::string view_transform;
    bool is_hdr = false;
    int fps = 24;
    std::string filename_pattern;
    int jitter_phases = 18;
    std::string tonemap_lut;
    std::string inverse_lut;
    std::map<std::string, CameraData> frames;
};

// OutputFormat enum: ExrLinear/Png16 image sequences and sequence-level video muxers are implemented in v2.
// The rest are enum placeholders for future extensions; DO NOT register video muxers to EncoderRegistry.
enum class OutputFormat {
    ExrLinear,
    Png16,
    ExrNrBuffer,  ///< DLSSNR guidance data (RGB half: mv.x, mv.y, NDC depth) — no color transform
    ProRes422HQ,
    H265Mp4,
    // not implemented in v2 - future extension hooks:
    Jpeg,
    H264Mp4,
    Av1Mp4,
    Avif,
    ProRes4444XQ,
    DNxHR
};

// DLSS scaling ratios per quality mode (SR section 3.7.1.1)
constexpr float DLSS_RATIO[] = {
    1.0f,   // DLAA
    1.3f,   // UltraQuality
    1.5f,   // Quality
    1.724f, // Balanced
    2.0f,   // Performance
    3.0f    // UltraPerformance
};

// Jitter phase counts per quality mode (SR section 3.7.1.1 minimum table)
constexpr int JITTER_PHASES[] = {
    8,   // DLAA
    14,  // UltraQuality
    18,  // Quality
    24,  // Balanced
    32,  // Performance
    72   // UltraPerformance
};

// Per-quality preset mapping (SR section 3.12)
// DLAA/UltraQuality/Quality/Balanced -> K; Performance -> M; UltraPerformance -> L
enum class NgxPreset { J = 'J', K = 'K', L = 'L', M = 'M' };
constexpr NgxPreset DEFAULT_PRESET[] = {
    NgxPreset::K, // DLAA
    NgxPreset::K, // UltraQuality
    NgxPreset::K, // Quality
    NgxPreset::K, // Balanced
    NgxPreset::M, // Performance
    NgxPreset::L  // UltraPerformance
};

} // namespace dlss
