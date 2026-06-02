// =============================================================================
// dlss-compositor-cli — CLI entry point.
//
// Modes:
//   --version       Print version and exit.
//   --help          Print usage and exit.
//   --self-check    Verify GPU + NGX availability (T36+ integration).
//   --rpc-stdio     Run JSON-RPC 2.0 server over stdio (Content-Length framed).
//   --config <p>    Process a dlssconfig.json (T36+ integration).
//
// Replaces the legacy main_smoke.cpp. The smoke entry remains in git history
// for reference but is no longer compiled.
// =============================================================================
#include "core/log.h"
#include "rpc/dispatcher.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace dlss {
// Provided by transport_stdio.cpp.
void runJsonRpcStdio(RpcDispatcher& dispatcher);
} // namespace dlss

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
        << "dlss-compositor-cli v2.0.0\n"
        << "Usage:\n"
        << "  --config <dlssconfig.json>   Process a config file (Wave 6).\n"
        << "  --rpc-stdio                  JSON-RPC 2.0 stdio server mode.\n"
        << "  --self-check                 Check GPU + NGX availability.\n"
        << "  --version                    Print version.\n"
        << "  --help                       This help message.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    dlss::log::init();

    const std::vector<std::string> args(argv + 1, argv + argc);

    if (hasArg(args, "--version")) {
        std::cerr << "dlss-compositor-cli v2.0.0 (DLSS SDK 310.6.0)\n";
        return 0;
    }

    if (hasArg(args, "--help") || args.empty()) {
        printHelp();
        return args.empty() ? 1 : 0;
    }

    if (hasArg(args, "--self-check")) {
        LOG_INFO("[self-check] DLSS Compositor v2 self-check");
        // TODO(T36+): boot Vulkan + NGX and report device, driver, NGX feature
        // availability. Today we only verify the logger pipeline is alive.
        LOG_INFO("[self-check] GPU: pending integration");
        LOG_INFO("[self-check] NGX SR: pending integration");
        LOG_INFO("[self-check] NGX FG: pending integration");
        return 0;
    }

    if (hasArg(args, "--rpc-stdio")) {
        // JSON-RPC 2.0 stdio server (Flutter plugin fallback + automation).
        dlss::RpcDispatcher dispatcher;
        dlss::runJsonRpcStdio(dispatcher);
        return 0;
    }

    const std::string configPath = getArg(args, "--config");
    if (!configPath.empty()) {
        LOG_INFO("[cli] Processing config: {}", configPath);
        // TODO(T36+): wire dlssconfig parser + pipeline executor.
        LOG_ERROR("[cli] Pipeline not yet integrated (Wave 6 tasks pending)");
        return 1;
    }

    std::cerr << "dlss-compositor-cli: no command specified. Use --help for usage.\n";
    return 1;
}
