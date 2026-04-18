#include <cstdio>
#include <cstdlib>
#include <string>

#include "cli/cli_parser.h"
#include "cli/config.h"
#include "core/logger.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "pipeline/sequence_processor.h"
#include "ui/app.h"

int main(int argc, char* argv[]) {
    Log::init();
    std::atexit(Log::shutdown);

    AppConfig config;
    std::string errorMsg;

    if (!CliParser::parse(argc, argv, config, errorMsg)) {
        Log::error("Error: %s\n", errorMsg.c_str());
        Log::error("Run with --help for usage.\n");
        return 1;
    }

    if (config.showHelp) {
        CliParser::printHelp();
        return 0;
    }
    if (config.showVersion) {
        CliParser::printVersion();
        return 0;
    }
    if (config.testVulkan) {
        VulkanContext context;
        if (!context.init(errorMsg)) {
            Log::error("%s\n", errorMsg.c_str());
            return 1;
        }

        Log::info("Vulkan initialized: %s\n", context.gpuName().c_str());
        context.destroy();
        return 0;
    }
    if (config.testNgx) {
        VulkanContext context;
        if (!context.init(errorMsg)) {
            Log::error("%s\n", errorMsg.c_str());
            return 1;
        }

        NgxContext ngx;
        if (!ngx.init(context.instance(),
                      context.physicalDevice(),
                      context.device(),
                      nullptr,
                      errorMsg)) {
            Log::error("%s\n", errorMsg.c_str());
            context.destroy();
            return 1;
        }

        if (ngx.isDlssSRAvailable()) {
            Log::info("DLSS-SR available: true\n");
            ngx.shutdown();
            context.destroy();
            return 0;
        }

        Log::error("DLSS-SR not available: %s\n", ngx.unavailableReason().c_str());
        ngx.shutdown();
        context.destroy();
        return 1;
    }

    if (config.testGui || config.launchGui) {
        VulkanContext computeCtx;
        if (!computeCtx.init(errorMsg)) {
            Log::error("Compute Vulkan Error: %s\n", errorMsg.c_str());
            return 1;
        }
        TexturePipeline pipeline(computeCtx);

        App app;
        if (!app.run(config, &computeCtx, &pipeline, errorMsg)) {
            Log::error("GUI Error: %s\n", errorMsg.c_str());
            computeCtx.destroy();
            return 1;
        }
        computeCtx.destroy();
        return 0;
    }

    if (!config.inputDir.empty() || !config.outputDir.empty()) {
        if (config.inputDir.empty() || config.outputDir.empty()) {
            Log::error("Error: both --input-dir and --output-dir are required for sequence processing.\n");
            return 1;
        }

        VulkanContext context;
        if (!context.init(errorMsg)) {
            Log::error("%s\n", errorMsg.c_str());
            return 1;
        }

        NgxContext ngx;
        if (!ngx.init(context.instance(),
                      context.physicalDevice(),
                      context.device(),
                      nullptr,
                      errorMsg)) {
            Log::error("%s\n", errorMsg.c_str());
            context.destroy();
            return 1;
        }

        if (config.interpolateFactor == 0 || config.scaleExplicit) {
            if (!ngx.isDlssSRAvailable()) {
                Log::error("DLSS-SR not available: %s\n", ngx.unavailableReason().c_str());
                ngx.shutdown();
                context.destroy();
                return 1;
            }
        }

        TexturePipeline texturePipeline(context);
        SequenceProcessor processor(context, ngx, texturePipeline);
        const bool ok = processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg);
        ngx.shutdown();
        context.destroy();

        if (!ok) {
            Log::error("%s\n", errorMsg.c_str());
            return 1;
        }

        return 0;
    }

    Log::info("dlss-compositor: no action specified. Use --help for usage.\n");
    return 0;
}
