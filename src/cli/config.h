#pragma once

#include "core/exr_writer.h"

#include <nvsdk_ngx_defs.h>

#include <cstdint>

#include <string>
enum class DlssQualityMode {
    DLAA,
    MaxQuality,
    Balanced,
    Performance,
    UltraPerformance
};

enum class DlssSRPreset {
    Auto = 0,
    J = 10,
    K = 11,
    L = 12,
    M = 13
};

enum class TonemapMode {
    None,        // No transport encoding — pass through scene-linear (may clamp HDR)
    PQ,          // PQ (ST 2084) per-channel encode/decode (default for FG)
    Custom       // User-provided forward/inverse LUT files
};

enum class ProgressFormat {
    Human,       // Default human-readable progress logging to stdout
    Json         // NDJSON (newline-delimited JSON) progress events to stdout
};

enum class OutputPass : uint32_t {
    Beauty = 1u << 0,
    Depth = 1u << 1,
    Normals = 1u << 2,
};

inline OutputPass operator|(OutputPass a, OutputPass b) {
    return static_cast<OutputPass>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline OutputPass operator&(OutputPass a, OutputPass b) {
    return static_cast<OutputPass>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasPass(OutputPass set, OutputPass flag) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0u;
}

struct AppConfig {
    std::string inputDir;
    std::string outputDir;

    float scaleFactor = 0.0f;
    bool scaleExplicit = false;
    DlssQualityMode quality = DlssQualityMode::MaxQuality;
    DlssSRPreset preset = DlssSRPreset::Auto;
    DlssSRPreset presetDLAA = DlssSRPreset::Auto;
    DlssSRPreset presetQuality = DlssSRPreset::Auto;
    DlssSRPreset presetBalanced = DlssSRPreset::Auto;
    DlssSRPreset presetPerformance = DlssSRPreset::Auto;
    DlssSRPreset presetUltraPerformance = DlssSRPreset::Auto;
    DlssSRPreset presetUltraQuality = DlssSRPreset::Auto;

    int interpolateFactor = 0;
    std::string cameraDataFile;
    int memoryBudgetGB = 8;

    std::string channelMapFile;

    bool encodeVideo = false;
    std::string videoOutputFile = "output.mp4";
    int fps = 24;
    int sourceFps = 30;  // Source footage frame rate (for DLSS frame time delta)

    bool testNgx = false;
    bool testVulkan = false;

    NVSDK_NGX_Logging_Level ngxLogLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    bool showHelp = false;
    bool showVersion = false;

    ExrCompression exrCompression = ExrCompression::Dwaa;
    float exrDwaQuality = 95.0f;
    OutputPass outputPasses = OutputPass::Beauty;

    float sharpness = 0.0f;  // Post-DLSS sharpness [0.0, 1.0]; 0.0 = bypass

    TonemapMode tonemapMode = TonemapMode::PQ;
    bool inverseTonemapEnabled = true;
    std::string forwardLutFile;
    std::string inverseLutFile;

    ProgressFormat progressFormat = ProgressFormat::Human;

    // NGX advanced (power-user) — all default to current behavior
    bool   featureFlagHDR           = true;   // NVSDK_NGX_DLSS_Feature_Flags_IsHDR
    bool   featureFlagMVLowRes      = true;   // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
    bool   featureFlagAutoExposure  = true;   // NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
    bool   enableOutputSubrects     = false;  // createParams.InEnableOutputSubrects
    float  exposureScale            = 1.0f;   // InExposureScale (and InPreExposure when autoExposure=false)
    float  frameTimeDeltaMsOverride = -1.0f;  // -1 = auto from --source-fps; >0 = manual override
    float  dlssSharpness            = 0.0f;   // NGX InSharpness [0,1]; 0 = off
    bool   enableDynamicResolution  = false;  // reserved/FG-only knob; stored but not yet wired to NGX
};
