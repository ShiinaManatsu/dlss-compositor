#include "config_executor.h"

#include "preview_ipc.h"

#include "../core/dlssconfig.h"
#include "../core/log.h"
#include "../dlss/ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../io/async_writer.h"
#include "../pipeline/fg_only_pipeline.h"
#include "../pipeline/sr_fg_pipeline.h"
#include "../pipeline/sr_only_pipeline.h"
#include "../rpc/dispatcher.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace dlss {

namespace {

OutputFormat parseOutputFormat(const std::string& value, bool isHdr) {
    if (value == "" || value == "exr" || value == "exr_linear") {
        return OutputFormat::ExrLinear;
    }
    if (value == "png" || value == "png_16") {
        if (isHdr) {
            throw std::runtime_error("PNG output is not supported for HDR projects");
        }
        return OutputFormat::Png16;
    }
    if (value == "prores" || value == "prores_422hq") {
        return OutputFormat::ProRes422HQ;
    }
    throw std::runtime_error("Unsupported output format: " + value);
}

OverwriteMode parseOverwriteMode(const std::string& value) {
    if (value == "" || value == "overwrite") {
        return OverwriteMode::Overwrite;
    }
    if (value == "skip") {
        return OverwriteMode::Skip;
    }
    if (value == "autoVersion" || value == "auto-version" || value == "auto_version") {
        return OverwriteMode::AutoVersion;
    }
    throw std::runtime_error("Unsupported overwrite mode: " + value);
}

std::filesystem::path defaultOutputDir(const std::filesystem::path& configPath) {
    const auto parent = configPath.parent_path();
    return parent.empty() ? std::filesystem::path("output") : parent / "output";
}

std::string defaultTemplateFor(OutputFormat format) {
    if (format == OutputFormat::ProRes422HQ) {
        return "output.mov";
    }
    if (format == OutputFormat::Png16) {
        return "frame_####.png";
    }
    return "frame_####.exr";
}

bool needsSr(const DlssConfig& config) {
    return config.render_resolution.width != config.output_resolution.width ||
           config.render_resolution.height != config.output_resolution.height;
}

int outputFrameCount(int inputFrames, bool fgEnabled, int fgMultiplier) {
    if (!fgEnabled) {
        return inputFrames;
    }
    return fgMultiplier * inputFrames - (fgMultiplier - 1);
}

void emitProgressEvent(int frame, int total) {
    std::cerr << "DLSS_EVENT {\"type\":\"progress\",\"current_frame\":"
              << frame << ",\"total_frames\":" << total << ",\"percent\":"
              << (total > 0 ? static_cast<double>(frame) / static_cast<double>(total) : 0.0)
              << "}\n";
}

void emitDoneEvent() {
    std::cerr << "DLSS_EVENT {\"type\":\"done\"}\n";
}

} // namespace

int executeConfig(const ConfigExecutionOptions& options) {
    auto loaded = DlssConfigLoader::Load(options.configPath);
    if (!loaded.has_value()) {
        const std::string message = loaded.error().message;
        const std::string path = loaded.error().path;
        LOG_ERROR("[cli] Failed to load dlssconfig: {} at {}",
                  message,
                  path);
        return 2;
    }

    const DlssConfig& config = loaded.value();
    const int inputFrames = static_cast<int>(config.frames.size());
    if (inputFrames <= 0) {
        LOG_ERROR("[cli] dlssconfig has no frames");
        return 3;
    }
    if (options.fgEnabled && inputFrames < 2) {
        LOG_ERROR("[cli] Frame Generation requires at least two input frames");
        return 5;
    }

    try {
        const OutputFormat format = parseOutputFormat(options.outputFormat, config.is_hdr);
        const OverwriteMode overwrite = parseOverwriteMode(options.overwriteMode);
        const std::filesystem::path outputDir = options.outputDir.empty()
            ? defaultOutputDir(options.configPath)
            : options.outputDir;
        const std::string filenameTemplate = options.outputTemplate.empty()
            ? defaultTemplateFor(format)
            : options.outputTemplate;
        PreviewPublisher preview(options.previewSharedMemoryName);
        auto previewCallback = [&preview](const std::vector<std::uint8_t>& pixels,
                                          std::uint32_t width,
                                          std::uint32_t height) {
            preview.publishRgba16f(pixels, width, height);
        };
        const int totalFrames = outputFrameCount(inputFrames, options.fgEnabled, options.fgMultiplier);
        const std::string outputDirString = outputDir.string();
        const std::string formatString = options.outputFormat.empty() ? std::string("exr") : options.outputFormat;

        LOG_INFO("[cli] Executing config: input_frames={} output_frames={} output_dir={} format={}",
                 inputFrames,
                 totalFrames,
                 outputDirString,
                 formatString);

        LOG_INFO("[cli] Creating Vulkan context");
        VulkanContext vulkan;
        LOG_INFO("[cli] Vulkan context ready");
        LOG_INFO("[cli] Creating NGX context");
        NgxContext ngx(vulkan.instance(), vulkan.physicalDevice(), vulkan.device());
        LOG_INFO("[cli] NGX context ready");
        RpcDispatcher dispatcher;

        auto runPipeline = [&] {
        if (options.fgEnabled && needsSr(config)) {
            SrFgConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.previewCallback = previewCallback;
            pipelineConfig.fgMultiplier = options.fgMultiplier;
            SrFgPipeline(&vulkan, &ngx, &dispatcher).run(pipelineConfig);
        } else if (options.fgEnabled) {
            FgOnlyConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.previewCallback = previewCallback;
            pipelineConfig.multiplier = options.fgMultiplier;
            FgOnlyPipeline(&vulkan, &ngx, &dispatcher).run(pipelineConfig);
        } else {
            SrOnlyConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.previewCallback = previewCallback;
            SrOnlyPipeline(&vulkan, &ngx, &dispatcher).run(pipelineConfig);
        }
        };

        if (options.emitEvents) {
            std::atomic<bool> done{false};
            std::exception_ptr failure;
            std::thread worker([&] {
                try {
                    runPipeline();
                } catch (...) {
                    failure = std::current_exception();
                }
                done.store(true);
            });

            int lastFrame = -1;
            while (!done.load()) {
                const int current = dispatcher.progress().current_frame.load();
                const int total = dispatcher.progress().total_frames.load();
                if (total > 0 && current != lastFrame) {
                    emitProgressEvent(current, total);
                    lastFrame = current;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            worker.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            const int current = dispatcher.progress().current_frame.load();
            const int total = dispatcher.progress().total_frames.load();
            if (total > 0 && current != lastFrame) {
                emitProgressEvent(current, total);
            }
        } else {
            runPipeline();
        }

        LOG_INFO("[cli] Complete: {} output frame(s)", totalFrames);
        if (options.emitEvents) {
            emitDoneEvent();
        }
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR("[cli] Config execution failed: {}", error.what());
        return 4;
    }
}

} // namespace dlss
