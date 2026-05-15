#include "cli/cli_parser.h"

#include "core/version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace {

bool isFlag(const char* arg) {
    return arg != nullptr && std::strncmp(arg, "--", 2) == 0;
}

bool parseInt(const char* value, int& out) {
    if (value == nullptr || *value == '\0') {
        return false;
    }

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parseFloat(const char* value, float& out) {
    if (value == nullptr || *value == '\0') {
        return false;
    }

    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

bool parseQuality(const char* value, DlssQualityMode& out) {
    if (std::strcmp(value, "DLAA") == 0) {
        out = DlssQualityMode::DLAA;
        return true;
    }
    if (std::strcmp(value, "MaxQuality") == 0) {
        out = DlssQualityMode::MaxQuality;
        return true;
    }
    if (std::strcmp(value, "Balanced") == 0) {
        out = DlssQualityMode::Balanced;
        return true;
    }
    if (std::strcmp(value, "Performance") == 0) {
        out = DlssQualityMode::Performance;
        return true;
    }
    if (std::strcmp(value, "UltraPerformance") == 0) {
        out = DlssQualityMode::UltraPerformance;
        return true;
    }
    return false;
}

bool parsePreset(const char* value, DlssSRPreset& out) {
    if (std::strcmp(value, "auto") == 0 || std::strcmp(value, "Auto") == 0) {
        out = DlssSRPreset::Auto;
        return true;
    }
    if (std::strcmp(value, "J") == 0) {
        out = DlssSRPreset::J;
        return true;
    }
    if (std::strcmp(value, "K") == 0) {
        out = DlssSRPreset::K;
        return true;
    }
    if (std::strcmp(value, "L") == 0) {
        out = DlssSRPreset::L;
        return true;
    }
    if (std::strcmp(value, "M") == 0) {
        out = DlssSRPreset::M;
        return true;
    }
    return false;
}

bool parsePresetFlag(int& index,
                     int argc,
                     char* argv[],
                     const char* flagName,
                     DlssSRPreset& out,
                     std::string& errorMsg) {
    if (index + 1 >= argc) {
        errorMsg = std::string(flagName) + " requires a value";
        return false;
    }
    if (!parsePreset(argv[++index], out)) {
        errorMsg = std::string(flagName) + " must be one of: auto, J, K, L, M";
        return false;
    }
    return true;
}

bool parseInterpolateFactor(const char* value, int& out) {
    if (std::strcmp(value, "2x") == 0 || std::strcmp(value, "2X") == 0) {
        out = 2;
        return true;
    }
    if (std::strcmp(value, "4x") == 0 || std::strcmp(value, "4X") == 0) {
        out = 4;
        return true;
    }
    return false;
}

const char* qualityToString(DlssQualityMode mode) {
    switch (mode) {
    case DlssQualityMode::DLAA: return "DLAA";
    case DlssQualityMode::MaxQuality: return "MaxQuality";
    case DlssQualityMode::Balanced: return "Balanced";
    case DlssQualityMode::Performance: return "Performance";
    case DlssQualityMode::UltraPerformance: return "UltraPerformance";
    }
    return "MaxQuality";
}

bool parseTonemapMode(const char* value, TonemapMode& out) {
    if (std::strcmp(value, "none") == 0) {
        out = TonemapMode::None;
        return true;
    }
    if (std::strcmp(value, "pq") == 0) {
        out = TonemapMode::PQ;
        return true;
    }
    if (std::strcmp(value, "custom") == 0) {
        out = TonemapMode::Custom;
        return true;
    }
    return false;
}

bool parseCompression(const char* value, ExrCompression& out) {
    if (std::strcmp(value, "none") == 0) {
        out = ExrCompression::None;
        return true;
    }
    if (std::strcmp(value, "zip") == 0) {
        out = ExrCompression::Zip;
        return true;
    }
    if (std::strcmp(value, "zips") == 0) {
        out = ExrCompression::Zips;
        return true;
    }
    if (std::strcmp(value, "piz") == 0) {
        out = ExrCompression::Piz;
        return true;
    }
    if (std::strcmp(value, "dwaa") == 0) {
        out = ExrCompression::Dwaa;
        return true;
    }
    if (std::strcmp(value, "dwab") == 0) {
        out = ExrCompression::Dwab;
        return true;
    }
    return false;
}

bool parseOutputPasses(const char* value, OutputPass& out) {
    if (value == nullptr || *value == '\0') {
        return false;
    }

    OutputPass parsed = static_cast<OutputPass>(0u);
    std::string remaining(value);
    size_t start = 0;

    while (start <= remaining.size()) {
        const size_t end = remaining.find(',', start);
        std::string token = remaining.substr(start, end == std::string::npos ? std::string::npos : end - start);

        const size_t first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return false;
        }

        const size_t last = token.find_last_not_of(" \t\r\n");
        token = token.substr(first, last - first + 1u);

        if (token == "beauty") {
            parsed = parsed | OutputPass::Beauty;
        } else if (token == "depth") {
            parsed = parsed | OutputPass::Depth;
        } else if (token == "normals") {
            parsed = parsed | OutputPass::Normals;
        } else {
            return false;
        }

        if (end == std::string::npos) {
            break;
        }

        start = end + 1u;
    }

    out = parsed;
    return static_cast<uint32_t>(parsed) != 0u;
}

bool parseNgxLogLevel(const char* value, NVSDK_NGX_Logging_Level& out) {
    if (std::strcmp(value, "off") == 0) { out = NVSDK_NGX_LOGGING_LEVEL_OFF; return true; }
    if (std::strcmp(value, "verbose") == 0) { out = NVSDK_NGX_LOGGING_LEVEL_VERBOSE; return true; }
    if (std::strcmp(value, "warn") == 0) { out = NVSDK_NGX_LOGGING_LEVEL_ON; return true; }
    if (std::strcmp(value, "error") == 0) { out = NVSDK_NGX_LOGGING_LEVEL_ON; return true; }
    if (std::strcmp(value, "info") == 0) { out = NVSDK_NGX_LOGGING_LEVEL_ON; return true; }
    return false;
}

} // namespace

bool CliParser::parse(int argc, char* argv[], AppConfig& config, std::string& errorMsg) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            config.showHelp = true;
            continue;
        }
        if (std::strcmp(arg, "--version") == 0) {
            config.showVersion = true;
            continue;
        }
        if (std::strcmp(arg, "--test-ngx") == 0) {
            config.testNgx = true;
            continue;
        }
        if (std::strcmp(arg, "--test-vulkan") == 0) {
            config.testVulkan = true;
            continue;
        }
        if (std::strcmp(arg, "--inspect-exr") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--inspect-exr requires a file path";
                return false;
            }
            config.inspectExrPath = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--input-dir") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--input-dir requires a value";
                return false;
            }
            config.inputDir = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--output-dir") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--output-dir requires a value";
                return false;
            }
            config.outputDir = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--scale") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--scale requires a value";
                return false;
            }
            float scale = 0.0f;
            if (!parseFloat(argv[++i], scale) || scale < 1.0f || scale > 8.0f) {
                errorMsg = "--scale must be between 1.0 and 8.0";
                return false;
            }
            config.scaleFactor = scale;
            config.scaleExplicit = true;
            continue;
        }
        if (std::strcmp(arg, "--interpolate") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--interpolate requires a value";
                return false;
            }
            int interpolate = 0;
            if (!parseInterpolateFactor(argv[++i], interpolate)) {
                errorMsg = std::string("Invalid --interpolate value '") + argv[i] + "'. Valid values: 2x, 4x";
                return false;
            }
            config.interpolateFactor = interpolate;
            continue;
        }
        if (std::strcmp(arg, "--camera-data") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--camera-data requires a value";
                return false;
            }
            config.cameraDataFile = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--memory-budget") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--memory-budget requires a value";
                return false;
            }
            int budget = 0;
            if (!parseInt(argv[++i], budget) || budget < 1) {
                errorMsg = "--memory-budget must be an integer >= 1";
                return false;
            }
            config.memoryBudgetGB = budget;
            continue;
        }
        if (std::strcmp(arg, "--quality") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--quality requires a value";
                return false;
            }
            if (!parseQuality(argv[++i], config.quality)) {
                errorMsg = "--quality must be one of: DLAA, MaxQuality, Balanced, Performance, UltraPerformance";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset", config.preset, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-dlaa") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-dlaa", config.presetDLAA, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-quality") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-quality", config.presetQuality, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-balanced") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-balanced", config.presetBalanced, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-performance") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-performance", config.presetPerformance, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-ultra-performance") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-ultra-performance", config.presetUltraPerformance, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--preset-ultra-quality") == 0) {
            if (!parsePresetFlag(i, argc, argv, "--preset-ultra-quality", config.presetUltraQuality, errorMsg)) {
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--channel-map") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--channel-map requires a value";
                return false;
            }
            config.channelMapFile = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--fps") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--fps requires a value";
                return false;
            }
            int fps = 0;
            if (!parseInt(argv[++i], fps)) {
                errorMsg = "--fps must be an integer";
                return false;
            }
            config.fps = fps;
            continue;
        }
        if (std::strcmp(arg, "--source-fps") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--source-fps requires a value";
                return false;
            }
            int sfps = 0;
            if (!parseInt(argv[++i], sfps) || sfps < 0) {
                errorMsg = "--source-fps must be a non-negative integer";
                return false;
            }
            config.sourceFps = sfps > 0 ? sfps : 30;  // zero protection: fallback to 30
            continue;
        }
        if (std::strcmp(arg, "--encode-video") == 0) {
            config.encodeVideo = true;
            if (i + 1 < argc && !isFlag(argv[i + 1])) {
                config.videoOutputFile = argv[++i];
            }
            continue;
        }
        if (std::strcmp(arg, "--exr-compression") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--exr-compression requires a value";
                return false;
            }
            if (!parseCompression(argv[++i], config.exrCompression)) {
                errorMsg = "--exr-compression must be one of: none, zip, zips, piz, dwaa, dwab";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--exr-dwa-quality") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--exr-dwa-quality requires a value";
                return false;
            }
            if (!parseFloat(argv[++i], config.exrDwaQuality)) {
                errorMsg = "--exr-dwa-quality must be a float";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--sharpness") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--sharpness requires a value";
                return false;
            }
            float sharpness = 0.0f;
            if (!parseFloat(argv[++i], sharpness)) {
                errorMsg = "--sharpness must be a float";
                return false;
            }
            config.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
            continue;
        }
        // ── NGX advanced (power-user) ───────────────────────────────────────
        if (std::strcmp(arg, "--feature-flag-hdr") == 0) {
            config.featureFlagHDR = true;
            continue;
        }
        if (std::strcmp(arg, "--no-feature-flag-hdr") == 0) {
            config.featureFlagHDR = false;
            continue;
        }
        if (std::strcmp(arg, "--feature-flag-mv-low-res") == 0) {
            config.featureFlagMVLowRes = true;
            continue;
        }
        if (std::strcmp(arg, "--no-feature-flag-mv-low-res") == 0) {
            config.featureFlagMVLowRes = false;
            continue;
        }
        if (std::strcmp(arg, "--feature-flag-auto-exposure") == 0) {
            config.featureFlagAutoExposure = true;
            continue;
        }
        if (std::strcmp(arg, "--no-feature-flag-auto-exposure") == 0) {
            config.featureFlagAutoExposure = false;
            continue;
        }
        if (std::strcmp(arg, "--enable-output-subrects") == 0) {
            config.enableOutputSubrects = true;
            continue;
        }
        if (std::strcmp(arg, "--no-enable-output-subrects") == 0) {
            config.enableOutputSubrects = false;
            continue;
        }
        if (std::strcmp(arg, "--exposure-scale") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--exposure-scale requires a value";
                return false;
            }
            float v = 0.0f;
            if (!parseFloat(argv[++i], v) || !(v > 0.0f && v <= 100.0f)) {
                errorMsg = "--exposure-scale must be a float in (0.0, 100.0]";
                return false;
            }
            config.exposureScale = v;
            continue;
        }
        if (std::strcmp(arg, "--frame-time-delta-ms") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--frame-time-delta-ms requires a value";
                return false;
            }
            float v = 0.0f;
            if (!parseFloat(argv[++i], v) || !(v > 0.0f)) {
                errorMsg = "--frame-time-delta-ms must be > 0.0 (use -1 to auto-derive from --source-fps)";
                return false;
            }
            config.frameTimeDeltaMsOverride = v;
            continue;
        }
        if (std::strcmp(arg, "--dlss-sharpness") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--dlss-sharpness requires a value";
                return false;
            }
            float v = 0.0f;
            if (!parseFloat(argv[++i], v) || v < 0.0f || v > 1.0f) {
                errorMsg = "--dlss-sharpness must be a float in [0.0, 1.0]";
                return false;
            }
            config.dlssSharpness = v;
            continue;
        }
        if (std::strcmp(arg, "--enable-dynamic-resolution") == 0) {
            config.enableDynamicResolution = true;
            continue;
        }
        if (std::strcmp(arg, "--no-enable-dynamic-resolution") == 0) {
            config.enableDynamicResolution = false;
            continue;
        }
        if (std::strcmp(arg, "--no-camera-cut-detection") == 0) {
            config.cameraCutDetectionEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--camera-cut-translation-threshold") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--camera-cut-translation-threshold requires a value";
                return false;
            }
            float v = 0.0f;
            if (!parseFloat(argv[++i], v) || !(v > 0.0f)) {
                errorMsg = "--camera-cut-translation-threshold must be > 0.0";
                return false;
            }
            config.cameraCutTranslationThreshold = v;
            continue;
        }
        if (std::strcmp(arg, "--camera-cut-rotation-threshold") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--camera-cut-rotation-threshold requires a value";
                return false;
            }
            float v = 0.0f;
            if (!parseFloat(argv[++i], v) || !(v > 0.0f && v <= 180.0f)) {
                errorMsg = "--camera-cut-rotation-threshold must be a float in (0.0, 180.0]";
                return false;
            }
            config.cameraCutRotationThreshold = v;
            continue;
        }
        if (std::strcmp(arg, "--output-passes") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--output-passes requires a value";
                return false;
            }
            if (!parseOutputPasses(argv[++i], config.outputPasses)) {
                errorMsg = "--output-passes must be a comma-separated list of: beauty, depth, normals";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--tonemap") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--tonemap requires a value";
                return false;
            }
            if (!parseTonemapMode(argv[++i], config.tonemapMode)) {
                errorMsg = "--tonemap must be one of: none, pq, custom";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--no-inverse-tonemap") == 0) {
            config.inverseTonemapEnabled = false;
            continue;
        }
        if (std::strcmp(arg, "--tonemap-lut") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--tonemap-lut requires a value";
                return false;
            }
            config.forwardLutFile = argv[++i];
            config.tonemapMode = TonemapMode::Custom;
            continue;
        }
        if (std::strcmp(arg, "--inverse-tonemap-lut") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--inverse-tonemap-lut requires a value";
                return false;
            }
            config.inverseLutFile = argv[++i];
            config.tonemapMode = TonemapMode::Custom;
            continue;
        }
        if (std::strcmp(arg, "--ngx-log-level") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--ngx-log-level requires a value";
                return false;
            }
            if (!parseNgxLogLevel(argv[++i], config.ngxLogLevel)) {
                errorMsg = "--ngx-log-level must be one of: off, error, warn, info, verbose";
                return false;
            }
            continue;
        }
        if (std::strcmp(arg, "--progress-format") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--progress-format requires a value";
                return false;
            }
            const char* fmt = argv[++i];
            if (std::strcmp(fmt, "human") == 0) {
                config.progressFormat = ProgressFormat::Human;
            } else if (std::strcmp(fmt, "json") == 0) {
                config.progressFormat = ProgressFormat::Json;
            } else {
                errorMsg = "--progress-format must be 'human' or 'json'";
                return false;
            }
            continue;
        }

        errorMsg = std::string("unknown argument: ") + arg;
        return false;
    }

    // Auto-discover camera.json from input directory
    if (config.cameraDataFile.empty() && !config.inputDir.empty()) {
        std::filesystem::path cameraPath = std::filesystem::path(config.inputDir) / "camera.json";
        if (std::filesystem::exists(cameraPath)) {
            config.cameraDataFile = cameraPath.string();
            std::fprintf(stderr, "[camera] auto-discovered: %s\n", config.cameraDataFile.c_str());
        }
    }

    if (config.interpolateFactor > 0 && config.cameraDataFile.empty()) {
        errorMsg = "--interpolate requires --camera-data";
        return false;
    }

    if (config.tonemapMode == TonemapMode::Custom) {
        if (config.forwardLutFile.empty()) {
            errorMsg = "--tonemap-lut is required when using custom LUT files";
            return false;
        }
        if (config.inverseTonemapEnabled && config.inverseLutFile.empty()) {
            errorMsg = "--inverse-tonemap-lut is required when using custom LUT with inverse enabled. "
                       "Use --no-inverse-tonemap to skip inverse, or provide --inverse-tonemap-lut.";
            return false;
        }
    }

    return true;
}

void CliParser::printHelp() {
    std::printf("dlss-compositor v%s\n", appver::kEngineVersion);
    std::printf("Usage: dlss-compositor [options]\n");
    std::printf("Options:\n");
    std::printf("  --input-dir <dir>      Input EXR sequence directory\n");
    std::printf("  --output-dir <dir>     Output EXR sequence directory\n");
    std::printf("  --scale <factor>       Upscale factor (float, 1.0–8.0; default 2.0). Can combine with --interpolate.\n");
    std::printf("  --interpolate <mode>   Frame interpolation (2x or 4x; requires --camera-data). Can combine with --scale.\n");
    std::printf("  --camera-data <file>   Camera metadata JSON file\n");
    std::printf("  --memory-budget <GB>   GPU memory budget for texture pool (default: 8, min: 1)\n");
    std::printf("  --quality <mode>       DLSS quality mode (DLAA|MaxQuality|Balanced|Performance|UltraPerformance)\n");
    std::printf("  --preset <mode>        SR preset hint (auto|J|K|L|M; default: auto, NVIDIA-recommended per quality)\n");
    std::printf("  --preset-dlaa <mode>   SR preset for DLAA (auto|J|K|L|M)\n");
    std::printf("  --preset-quality <mode> SR preset for MaxQuality (auto|J|K|L|M)\n");
    std::printf("  --preset-balanced <mode> SR preset for Balanced (auto|J|K|L|M)\n");
    std::printf("  --preset-performance <mode> SR preset for Performance (auto|J|K|L|M)\n");
    std::printf("  --preset-ultra-performance <mode> SR preset for UltraPerformance (auto|J|K|L|M)\n");
    std::printf("  --preset-ultra-quality <mode> SR preset for UltraQuality hint slot (auto|J|K|L|M)\n");
    std::printf("  --channel-map <file>   Custom channel name mapping JSON file\n");
    std::printf("  --encode-video [file]  Encode output sequence to MP4 via FFmpeg\n");
    std::printf("  --fps <rate>           Video encoding frame rate (default: 24)\n");
    std::printf("  --source-fps <rate>    Source footage frame rate for DLSS frame timing (default: 30)\n");
    std::printf("  --exr-compression <m>  EXR compression (none|zip|zips|piz|dwaa|dwab)\n");
    std::printf("  --exr-dwa-quality <f>  DWA compression quality (default: 95)\n");
    std::printf("  --sharpness <0.0..1.0> Post-DLSS sharpening (default 0.0 = off, recommended 0.2-0.4)\n");
    std::printf("  --output-passes <list> Output passes (comma-separated: beauty,depth,normals; default: beauty)\n");
    std::printf("  --tonemap <mode>       FG transport encoding (none|pq|custom; default: pq)\n");
    std::printf("  --no-inverse-tonemap   Skip inverse PQ decode on FG output (output stays PQ-encoded)\n");
    std::printf("  --tonemap-lut <file>   Custom forward LUT binary (implies --tonemap custom)\n");
    std::printf("  --inverse-tonemap-lut <file>  Custom inverse LUT binary\n");
    std::printf("  --ngx-log-level <level> NGX log verbosity (off|error|warn|info|verbose; default: off)\n");
    std::printf("  --progress-format <fmt> Progress reporting format (human|json; default: human)\n"
                "                          'json' emits NDJSON events to stdout (one JSON object per line)\n");
    std::printf("\n");
    std::printf("Advanced NGX settings (power-user; defaults match prior hardcoded behavior):\n");
    std::printf("  --feature-flag-hdr / --no-feature-flag-hdr               HDR feature flag (default: enabled)\n");
    std::printf("  --feature-flag-mv-low-res / --no-feature-flag-mv-low-res Render-resolution motion vectors (default: enabled)\n");
    std::printf("  --feature-flag-auto-exposure / --no-feature-flag-auto-exposure  NGX auto-exposure (default: enabled)\n");
    std::printf("  --enable-output-subrects / --no-enable-output-subrects   Output subrect support (default: disabled)\n");
    std::printf("  --exposure-scale <float>      InExposureScale; (0.0, 100.0] (default: 1.0)\n");
    std::printf("  --frame-time-delta-ms <float> Override frame time delta in ms; > 0.0 (default: auto from --source-fps)\n");
    std::printf("  --dlss-sharpness <0.0..1.0>   NGX InSharpness (default: 0.0 = off)\n");
    std::printf("  --enable-dynamic-resolution / --no-enable-dynamic-resolution  Reserved knob (default: disabled)\n");
    std::printf("  --no-camera-cut-detection               Disable camera cut detection (default: enabled)\n");
    std::printf("  --camera-cut-translation-threshold <f>  World-unit translation threshold for cut detection (default: 5.0)\n");
    std::printf("  --camera-cut-rotation-threshold <f>     Rotation threshold in degrees for cut detection (default: 30.0)\n");
    std::printf("\n");
    std::printf("  --test-ngx             Test NGX/DLSS-SR availability and exit\n");
    std::printf("  --test-vulkan         Initialize Vulkan compute context and exit\n");
    std::printf("  --inspect-exr <file>   Inspect EXR file header and output metadata as JSON\n");
    std::printf("  --help, -h             Show this help\n");
    std::printf("  --version              Show version\n");
}

void CliParser::printVersion() {
    std::printf("dlss-compositor %s\n", appver::kEngineVersion);
}
