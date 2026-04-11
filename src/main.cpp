#include <cstdio>
#include <string>

#include "cli/cli_parser.h"
#include "cli/config.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"
#include "pipeline/sequence_processor.h"
#include "ui/app.h"

int main(int argc, char* argv[]) {
    AppConfig config;
    std::string errorMsg;

    if (!CliParser::parse(argc, argv, config, errorMsg)) {
        fprintf(stderr, "Error: %s\n", errorMsg.c_str());
        fprintf(stderr, "Run with --help for usage.\n");
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
            fprintf(stderr, "%s\n", errorMsg.c_str());
            return 1;
        }

        printf("Vulkan initialized: %s\n", context.gpuName().c_str());
        context.destroy();
        return 0;
    }
    if (config.testNgx) {
        VulkanContext context;
        if (!context.init(errorMsg)) {
            fprintf(stderr, "%s\n", errorMsg.c_str());
            return 1;
        }

        NgxContext ngx;
        if (!ngx.init(context.instance(),
                      context.physicalDevice(),
                      context.device(),
                      nullptr,
                      errorMsg)) {
            fprintf(stderr, "%s\n", errorMsg.c_str());
            context.destroy();
            return 1;
        }

        if (ngx.isDlssRRAvailable()) {
            printf("DLSS-RR available: true\n");
            ngx.shutdown();
            context.destroy();
            return 0;
        }

        fprintf(stderr, "DLSS-RR not available: %s\n", ngx.unavailableReason().c_str());
        ngx.shutdown();
        context.destroy();
        return 1;
    }

    if (config.testGui || config.launchGui) {
        VulkanContext computeCtx;
        if (!computeCtx.init(errorMsg)) {
            fprintf(stderr, "Compute Vulkan Error: %s\n", errorMsg.c_str());
            return 1;
        }
        TexturePipeline pipeline(computeCtx);

        App app;
        if (!app.run(config, &computeCtx, &pipeline, errorMsg)) {
            fprintf(stderr, "GUI Error: %s\n", errorMsg.c_str());
            computeCtx.destroy();
            return 1;
        }
        computeCtx.destroy();
        return 0;
    }

    if (!config.inputDir.empty() || !config.outputDir.empty()) {
        if (config.inputDir.empty() || config.outputDir.empty()) {
            fprintf(stderr, "Error: both --input-dir and --output-dir are required for sequence processing.\n");
            return 1;
        }

        VulkanContext context;
        if (!context.init(errorMsg)) {
            fprintf(stderr, "%s\n", errorMsg.c_str());
            return 1;
        }

        NgxContext ngx;
        if (!ngx.init(context.instance(),
                      context.physicalDevice(),
                      context.device(),
                      nullptr,
                      errorMsg)) {
            fprintf(stderr, "%s\n", errorMsg.c_str());
            context.destroy();
            return 1;
        }

        if (config.interpolateFactor == 0 || config.scaleExplicit) {
            if (!ngx.isDlssRRAvailable()) {
                fprintf(stderr, "DLSS-RR not available: %s\n", ngx.unavailableReason().c_str());
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
            fprintf(stderr, "%s\n", errorMsg.c_str());
            return 1;
        }

        return 0;
    }

    printf("dlss-compositor: no action specified. Use --help for usage.\n");
    return 0;
}
