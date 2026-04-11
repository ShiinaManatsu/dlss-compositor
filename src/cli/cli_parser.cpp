#include "cli/cli_parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
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
    case DlssQualityMode::MaxQuality: return "MaxQuality";
    case DlssQualityMode::Balanced: return "Balanced";
    case DlssQualityMode::Performance: return "Performance";
    case DlssQualityMode::UltraPerformance: return "UltraPerformance";
    }
    return "Balanced";
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
        if (std::strcmp(arg, "--gui") == 0) {
            config.launchGui = true;
            continue;
        }
        if (std::strcmp(arg, "--test-gui") == 0) {
            config.testGui = true;
            continue;
        }
        if (std::strcmp(arg, "--test-process") == 0) {
            config.testGuiProcess = true;
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
            int scale = 0;
            if (!parseInt(argv[++i], scale) || (scale != 2 && scale != 3 && scale != 4)) {
                errorMsg = "--scale must be 2, 3, or 4";
                return false;
            }
            config.scaleFactor = scale;
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
        if (std::strcmp(arg, "--quality") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--quality requires a value";
                return false;
            }
            if (!parseQuality(argv[++i], config.quality)) {
                errorMsg = "--quality must be one of: MaxQuality, Balanced, Performance, UltraPerformance";
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
        if (std::strcmp(arg, "--test-load") == 0) {
            if (i + 1 >= argc) {
                errorMsg = "--test-load requires a value";
                return false;
            }
            config.testGuiLoad = argv[++i];
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

        errorMsg = std::string("unknown argument: ") + arg;
        return false;
    }

    if (config.interpolateFactor > 0 && config.cameraDataFile.empty()) {
        errorMsg = "--interpolate requires --camera-data";
        return false;
    }

    return true;
}

void CliParser::printHelp() {
    std::printf("dlss-compositor v0.1.0\n");
    std::printf("Usage: dlss-compositor [options]\n");
    std::printf("Options:\n");
    std::printf("  --input-dir <dir>      Input EXR sequence directory\n");
    std::printf("  --output-dir <dir>     Output EXR sequence directory\n");
    std::printf("  --scale <factor>       Upscale factor (2, 3, or 4)\n");
    std::printf("  --interpolate <mode>   Frame interpolation (2x or 4x; requires --camera-data)\n");
    std::printf("  --camera-data <file>   Camera metadata JSON file\n");
    std::printf("  --quality <mode>       DLSS quality mode (MaxQuality|Balanced|Performance|UltraPerformance)\n");
    std::printf("  --channel-map <file>   Custom channel name mapping JSON file\n");
    std::printf("  --encode-video [file]  Encode output sequence to MP4 via FFmpeg\n");
    std::printf("  --fps <rate>           Frame rate for video encoding (default: 24)\n");
    std::printf("  --exr-compression <m>  EXR compression (none|zip|zips|piz|dwaa|dwab)\n");
    std::printf("  --exr-dwa-quality <f>  DWA compression quality (default: 95)\n");
    std::printf("  --output-passes <list> Output passes (comma-separated: beauty,depth,normals; default: beauty)\n");
    std::printf("  --test-ngx             Test NGX/DLSS-RR availability and exit\n");
    std::printf("  --test-vulkan         Initialize Vulkan compute context and exit\n");
    std::printf("  --gui                  Launch ImGui viewer\n");
    std::printf("  --test-gui             Non-interactive GUI smoke test\n");
    std::printf("  --test-load <exr>      Load an EXR file for GUI smoke test\n");
    std::printf("  --test-process         Process via GUI test path\n");
    std::printf("  --help, -h             Show this help\n");
    std::printf("  --version              Show version\n");
}

void CliParser::printVersion() {
    std::printf("dlss-compositor 0.1.0\n");
}
