// =============================================================================
// dlss-compositor-cli — CLI entry point.
//
// Modes:
//   --version       Print version and exit.
//   --help          Print usage and exit.
//   --self-check    Verify GPU + NGX availability.
//   --config <p>    Process a dlssconfig.json (batch / config-file mode).
//   --rpc-stdio     Start the JSON-RPC 2.0 stdio server (run.py / Electron).
// =============================================================================
#include "core/log.h"
#include "cli/config_executor.h"
#include "gpu/vulkan_context.h"
#include "dlss/ngx_wrapper.h"
#include "rpc/dispatcher.h"

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
        << "  --rpc-stdio                  Start JSON-RPC 2.0 server on stdin/stdout.\n"
        << "  --config <dlssconfig.json>   Process a config file (batch mode).\n"
        << "  --output-dir <path>          Output directory for --config.\n"
        << "  --output-format <fmt>        exr | png_16 | prores_422hq | h265_hevc.\n"
        << "  --preset <auto|J|K|L|M>      DLSS SR render preset override (default auto).\n"
        << "  --output-template <tmpl>     Filename template, e.g. frame_####.exr.\n"
        << "  --overwrite-mode <mode>      overwrite | skip | autoVersion.\n"
        << "  --png-quality <0-100>        PNG zlib compression percent (default 97).\n"
        << "  --exr-compression <mode>     none | zip | dwaa (default zip).\n"
        << "  --exr-compression-level <0-100>  EXR zip/DWAA compression level (default 97).\n"
        << "  --writer-threads <1-32>      Image sequence write concurrency (default 4).\n"
        << "  --writer-buffer-multiplier <1-64>  Max queued frames (default 3).\n"
        << "  --h265-cq <0-30>            H.265 NVENC constant quality (default 18).\n"
        << "  --h265-preset <p1..p7>      H.265 NVENC speed/quality preset (default p5).\n"
        << "  --h265-multipass <mode>     disabled | qres | fullres (default qres).\n"
        << "  --h265-spatial-aq <0|1>     H.265 spatial adaptive quantization (default 1).\n"
        << "  --h265-temporal-aq <0|1>    H.265 temporal adaptive quantization (default 1).\n"
        << "  --h265-vfr <0|1>            Reserved VFR intent flag.\n"
        << "  --frame-start <n>            First input frame (inclusive).\n"
        << "  --frame-end <n>              Last input frame (inclusive).\n"
        << "  --fg <2x..6x>                Frame generation multiplier.\n"
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

    // ── --self-check ────────────────────────────────────────────────────────
    if (hasArg(args, "--self-check")) {
        LOG_INFO("[startup] CLI mode=self-check");
        LOG_INFO("[self-check] DLSS Compositor 1.0.0 self-check");
        try {
            dlss::VulkanContext vk;
            LOG_INFO("[self-check] Vulkan OK device={:p}", static_cast<void*>(vk.device()));
            dlss::NgxContext ngx(vk.instance(), vk.physicalDevice(), vk.device());
            const auto vers = ngx.getDllVersions();
            LOG_INFO("[self-check] NGX SR={} FG={} RR={}", ngx.isSrAvailable(), ngx.isFgAvailable(), ngx.isRrAvailable());
            LOG_INFO("[self-check] DLL versions SR={} FG={} RR={}", vers.sr, vers.fg, vers.rr);
            return 0;
        } catch (const std::exception& e) {
            LOG_ERROR("[self-check] failed: {}", std::string(e.what()));
            return 1;
        }
    }

    // ── --rpc-stdio ─────────────────────────────────────────────────────────
    if (hasArg(args, "--rpc-stdio")) {
        LOG_INFO("[startup] CLI mode=rpc-stdio");

        // Boot Vulkan + NGX.  These objects must outlive the dispatcher and
        // the JSON-RPC loop, so they are declared BEFORE the dispatcher
        // (reverse destruction order: dispatcher → ngx → vk).  The dispatcher
        // holds NR tool sessions that tear down GPU passes on exit.
        std::unique_ptr<dlss::VulkanContext> vk;
        std::unique_ptr<dlss::NgxContext>    ngx;

        // On failure, the server still starts but start_job / getStatus will
        // report the error.
        dlss::RpcDispatcher dispatcher;
        try {
            vk  = std::make_unique<dlss::VulkanContext>();
            ngx = std::make_unique<dlss::NgxContext>(vk->instance(),
                                                     vk->physicalDevice(),
                                                     vk->device());
            dispatcher.bindGpu(vk.get(), ngx.get());
            LOG_INFO("[rpc-stdio] GPU ready SR={} FG={} RR={}",
                     ngx->isSrAvailable(), ngx->isFgAvailable(), ngx->isRrAvailable());
        } catch (const std::exception& e) {
            LOG_ERROR("[rpc-stdio] GPU init failed (will serve RPC in degraded mode): {}",
                      std::string(e.what()));
            // Continue — clients can still call getStatus to learn the error.
        }

        dlss::runJsonRpcStdio(dispatcher);
        return 0;
    }

    // ── --config ────────────────────────────────────────────────────────────
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
        options.presetOverride = getArg(args, "--preset", "auto");
        options.overwriteMode = getArg(args, "--overwrite-mode", "overwrite");
        options.pngQualityPercent = std::stoi(getArg(args, "--png-quality", "97"));
        options.exrCompression = getArg(args, "--exr-compression", "zip");
        options.exrCompressionLevelPercent = std::stoi(getArg(args, "--exr-compression-level", "97"));
        options.writerThreads = std::stoi(getArg(args, "--writer-threads", "4"));
        options.writerQueueMultiplier = std::stoi(getArg(args, "--writer-buffer-multiplier", "3"));
        options.h265Options.cq = std::stoi(getArg(args, "--h265-cq", "18"));
        options.h265Options.preset = getArg(args, "--h265-preset", "p5");
        options.h265Options.multipass = getArg(args, "--h265-multipass", "qres");
        options.h265Options.spatialAq = getArg(args, "--h265-spatial-aq", "1") != "0";
        options.h265Options.temporalAq = getArg(args, "--h265-temporal-aq", "1") != "0";
        options.h265Options.vfr = getArg(args, "--h265-vfr", "0") != "0";
        const std::string frameStart = getArg(args, "--frame-start");
        const std::string frameEnd   = getArg(args, "--frame-end");
        if (!frameStart.empty()) options.frameStart = std::stoi(frameStart);
        if (!frameEnd.empty())   options.frameEnd   = std::stoi(frameEnd);
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
