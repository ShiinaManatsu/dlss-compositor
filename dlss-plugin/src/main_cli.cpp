// =============================================================================
// dlss-compositor-cli — CLI entry point.
//
// Modes:
//   --version       Print version and exit.
//   --help          Print usage and exit.
//   --self-check    Verify GPU + NGX availability (T36+ integration).
//   --config <p>    Process a dlssconfig.json (T36+ integration).
//
// Replaces the legacy main_smoke.cpp. The smoke entry remains in git history
// for reference but is no longer compiled.
// =============================================================================
#include "core/log.h"
#include "cli/config_executor.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool hasArg(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

std::string getArg(const std::vector<std::string>& args,
                   const std::string& flag,
                   const std::string& fallback = std::string{}) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return fallback;
}

void printHelp() {
    std::cerr
        << "dlss-compositor-cli v1.0.0\n"
        << "Usage:\n"
        << "  --config <dlssconfig.json>   Process a config file (Wave 6).\n"
        << "  --output-dir <path>          Output directory for --config.\n"
        << "  --output-format <fmt>        exr | png_16 | prores_422hq (PNG is LDR only; ProRes supports LDR/HDR).\n"
        << "  --output-template <tmpl>     Filename template, e.g. frame_####.exr.\n"
        << "  --overwrite-mode <mode>      overwrite | skip | autoVersion.\n"
        << "  --png-quality <0-100>        PNG zlib compression percent (default 97).\n"
        << "  --exr-compression <mode>     none | zip | dwaa (default zip).\n"
        << "  --exr-compression-level <0-100>  EXR zip/DWAA compression level (default 97).\n"
        << "  --writer-threads <1-32>      Image sequence write concurrency (default 4).\n"
        << "  --writer-buffer-multiplier <1-20>  Max queued frames = threads * multiplier (default 3).\n"
        << "  --fg <2x..6x>                Enable frame generation multiplier.\n"
        << "  --events stderr              Emit structured progress events on stderr.\n"
        << "  --self-check                 Check GPU + NGX availability.\n"
        << "  --version                    Print version.\n"
        << "  --help                       This help message.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    dlss::log::init(true);
    LOG_INFO("[startup] CLI launched argc={}", argc);

    const std::vector<std::string> args(argv + 1, argv + argc);
    LOG_INFO("[startup] CLI parsed {} argument(s)", args.size());

    if (hasArg(args, "--version")) {
        LOG_INFO("[startup] CLI mode=version");
        std::cerr << "dlss-compositor-cli v1.0.0 (DLSS SDK 310.6.0)\n";
        return 0;
    }

    if (hasArg(args, "--help") || args.empty()) {
        LOG_INFO("[startup] CLI mode=help empty_args={}", args.empty());
        printHelp();
        return args.empty() ? 1 : 0;
    }

    if (hasArg(args, "--self-check")) {
        LOG_INFO("[startup] CLI mode=self-check");
        LOG_INFO("[self-check] DLSS Compositor 1.0.0 self-check");
        // TODO(T36+): boot Vulkan + NGX and report device, driver, NGX feature
        // availability. Today we only verify the logger pipeline is alive.
        LOG_INFO("[self-check] GPU: pending integration");
        LOG_INFO("[self-check] NGX SR: pending integration");
        LOG_INFO("[self-check] NGX FG: pending integration");
        return 0;
    }

    const std::string configPath = getArg(args, "--config");
    if (!configPath.empty()) {
        LOG_INFO("[startup] CLI mode=config");
        LOG_INFO("[cli] Processing config: {}", configPath);
        const std::string fg = getArg(args, "--fg");
        dlss::ConfigExecutionOptions options;
        options.configPath = configPath;
        options.outputDir = getArg(args, "--output-dir", getArg(args, "--output"));
        options.outputTemplate = getArg(args, "--output-template", getArg(args, "--template"));
        options.outputFormat = getArg(args, "--output-format", "exr");
        options.overwriteMode = getArg(args, "--overwrite-mode", "overwrite");
        options.pngQualityPercent = std::stoi(getArg(args, "--png-quality", "97"));
        options.exrCompression = getArg(args, "--exr-compression", "zip");
        options.exrCompressionLevelPercent = std::stoi(getArg(args, "--exr-compression-level", "97"));
        options.writerThreads = std::stoi(getArg(args, "--writer-threads", "4"));
        options.writerQueueMultiplier = std::stoi(getArg(args, "--writer-buffer-multiplier", "3"));
        options.previewSharedMemoryName = getArg(args, "--preview-shm");
        options.emitEvents = getArg(args, "--events") == "stderr";
        options.fgEnabled = !fg.empty();
        if (fg == "2x" || fg == "2") options.fgMultiplier = 2;
        else if (fg == "3x" || fg == "3") options.fgMultiplier = 3;
        else if (fg == "4x" || fg == "4") options.fgMultiplier = 4;
        else if (fg == "5x" || fg == "5") options.fgMultiplier = 5;
        else if (fg == "6x" || fg == "6") options.fgMultiplier = 6;
        else if (!fg.empty()) {
            LOG_ERROR("[cli] Unsupported --fg value: {}", fg);
            return 4;
        }
        return dlss::executeConfig(options);
    }

    LOG_WARN("[startup] CLI mode=missing-command");
    std::cerr << "dlss-compositor-cli: no command specified. Use --help for usage.\n";
    return 1;
}
