#include "config_executor.h"

#include "preview_ipc.h"

#include "../core/dlssconfig.h"
#include "../core/log.h"
#include "../dlss/ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../io/async_writer.h"
#include "../io/prores_writer.h"
#include "../pipeline/fg_only_pipeline.h"
#include "../pipeline/sr_fg_pipeline.h"
#include "../pipeline/sr_only_pipeline.h"
#include "../rpc/dispatcher.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <regex>
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
        (void)isHdr;
        return OutputFormat::ProRes422HQ;
    }
    if (value == "h265" || value == "hevc" || value == "h265_hevc" || value == "h265_mp4") {
        (void)isHdr;
        return OutputFormat::H265Mp4;
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

ExrCompressionMode parseExrCompression(const std::string& value) {
    if (value == "" || value == "zip") {
        return ExrCompressionMode::Zip;
    }
    if (value == "none" || value == "uncompressed") {
        return ExrCompressionMode::None;
    }
    if (value == "dwaa") {
        return ExrCompressionMode::Dwaa;
    }
    throw std::runtime_error("Unsupported EXR compression mode: " + value);
}

DlssPreset parsePresetOverride(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value.empty() || value == "auto") {
        return DlssPreset::Auto;
    }
    if (value == "j") return DlssPreset::J;
    if (value == "k") return DlssPreset::K;
    if (value == "l") return DlssPreset::L;
    if (value == "m") return DlssPreset::M;
    throw std::runtime_error("Unsupported DLSS preset: " + value);
}

std::filesystem::path defaultOutputDir(const std::filesystem::path& configPath) {
    const auto parent = configPath.parent_path();
    return parent.empty() ? std::filesystem::path("output") : parent / "output";
}

std::string defaultTemplateFor(OutputFormat format) {
    if (format == OutputFormat::Png16) {
        return "frame_####.png";
    }
    if (format == OutputFormat::ProRes422HQ) {
        return "output.mov";
    }
    if (format == OutputFormat::H265Mp4) {
        return "output.mp4";
    }
    return "frame_####.exr";
}

std::string extensionFor(OutputFormat format) {
    if (format == OutputFormat::Png16) {
        return ".png";
    }
    if (format == OutputFormat::ProRes422HQ) {
        return ".mov";
    }
    if (format == OutputFormat::H265Mp4) {
        return ".mp4";
    }
    return ".exr";
}

std::string withFormatExtension(std::string filenameTemplate, OutputFormat format) {
    if (filenameTemplate.empty()) {
        return defaultTemplateFor(format);
    }
    const std::string ext = extensionFor(format);
    const auto dot = filenameTemplate.find_last_of('.');
    if (dot != std::string::npos) {
        const std::string currentExt = filenameTemplate.substr(dot);
        if (currentExt == ".png" || currentExt == ".exr" || currentExt == ".mov" || currentExt == ".mp4") {
            filenameTemplate.replace(dot, std::string::npos, ext);
            return filenameTemplate;
        }
    }
    return filenameTemplate + ext;
}

bool needsSr(const DlssConfig& config) {
    return config.render_resolution.width != config.output_resolution.width ||
           config.render_resolution.height != config.output_resolution.height;
}

bool isVideoFormat(OutputFormat format) {
    return format == OutputFormat::ProRes422HQ || format == OutputFormat::H265Mp4;
}

std::string videoOutputName(std::string filenameTemplate, OutputFormat format) {
    filenameTemplate = withFormatExtension(std::move(filenameTemplate), format);
    std::string out;
    for (std::size_t i = 0; i < filenameTemplate.size(); ++i) {
        if (filenameTemplate[i] == '#') {
            continue;
        }
        out.push_back(filenameTemplate[i]);
    }
    out = std::regex_replace(out, std::regex(R"(\{frame(?::0?\d+d)?\})"), "");
    while (out.find("__") != std::string::npos) {
        out = std::regex_replace(out, std::regex("__"), "_");
    }
    const auto dot = out.find_last_of('.');
    const std::string ext = extensionFor(format);
    if (dot == 0 || out.empty() || out == ext) {
        return defaultTemplateFor(format);
    }
    return out;
}

int outputFrameCount(int inputFrames, bool fgEnabled, int fgMultiplier) {
    if (!fgEnabled) {
        return inputFrames;
    }
    return fgMultiplier * inputFrames - (fgMultiplier - 1);
}

DlssConfig selectFrameRange(DlssConfig config, int frameStart, int frameEnd) {
    if (frameStart < 0 && frameEnd < 0) {
        return config;
    }
    if (frameStart >= 0 && frameEnd >= 0 && frameStart > frameEnd) {
        throw std::runtime_error("Frame range start must be <= end");
    }

    std::map<std::string, CameraData> selected;
    for (const auto& [key, camera] : config.frames) {
        int frameNumber = 0;
        try {
            frameNumber = std::stoi(key);
        } catch (...) {
            LOG_WARN("[cli] Ignoring non-numeric frame key during range selection: {}", key);
            continue;
        }
        if (frameStart >= 0 && frameNumber < frameStart) {
            continue;
        }
        if (frameEnd >= 0 && frameNumber > frameEnd) {
            continue;
        }
        selected.emplace(key, camera);
    }
    config.frames = std::move(selected);
    return config;
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

int executeConfig(const ConfigExecutionOptions& options, VulkanContext& vulkan, NgxContext& ngx) {
    auto loaded = DlssConfigLoader::Load(options.configPath);
    if (!loaded.has_value()) {
        const std::string message = loaded.error().message;
        const std::string path = loaded.error().path;
        LOG_ERROR("[cli] Failed to load dlssconfig: {} at {}",
                  message,
                  path);
        return 2;
    }

    const DlssConfig config = selectFrameRange(
        loaded.value(),
        options.frameStart,
        options.frameEnd);
    const int inputFrames = static_cast<int>(config.frames.size());
    if (inputFrames <= 0) {
        LOG_ERROR("[cli] dlssconfig has no frames in requested range start={} end={}",
                  options.frameStart,
                  options.frameEnd);
        return 3;
    }
    if (options.fgEnabled && inputFrames < 2) {
        LOG_ERROR("[cli] Frame Generation requires at least two input frames");
        return 5;
    }

    try {
        const OutputFormat format = parseOutputFormat(options.outputFormat, config.is_hdr);
        const OverwriteMode overwrite = parseOverwriteMode(options.overwriteMode);
        const EncoderOptions encoderOptions{
            std::clamp(options.pngQualityPercent, 0, 100),
            parseExrCompression(options.exrCompression),
            std::clamp(options.exrCompressionLevelPercent, 0, 100),
        };
        const std::filesystem::path outputDir = options.outputDir.empty()
            ? defaultOutputDir(options.configPath)
            : options.outputDir;
        const std::string filenameTemplate = isVideoFormat(format)
            ? videoOutputName(options.outputTemplate, format)
            : withFormatExtension(options.outputTemplate, format);
        PreviewPublisher preview(options.previewSharedMemoryName);
        auto previewCallback = [&preview, &options](const std::vector<std::uint8_t>& pixels,
                                                    std::uint32_t width,
                                                    std::uint32_t height) {
            if (!preview.enabled() && !options.previewCallback) {
                return;
            }
            if (options.wantsPreviewFrame && !options.wantsPreviewFrame()) {
                return;
            }
            if (options.previewCallback) {
                options.previewCallback(pixels, width, height);
            }
            preview.publishRgba16f(pixels, width, height);
        };
        const int totalFrames = outputFrameCount(inputFrames, options.fgEnabled, options.fgMultiplier);
        const std::string outputDirString = outputDir.string();
        const std::string formatString = options.outputFormat.empty() ? std::string("exr") : options.outputFormat;
        const int writerThreads = std::clamp(options.writerThreads, 1, 32);
        const int writerQueueMultiplier = std::clamp(options.writerQueueMultiplier, 1, 64);
        const DlssPreset presetOverride = parsePresetOverride(options.presetOverride);

        LOG_INFO("[cli] Executing config: input_frames={} output_frames={} output_dir={} format={} writer_threads={} queue_multiplier={} frame_start={} frame_end={} preset={}",
                 inputFrames,
                 totalFrames,
                 outputDirString,
                 formatString,
                 writerThreads,
                 writerQueueMultiplier,
                 options.frameStart,
                 options.frameEnd,
                 options.presetOverride);

        LOG_INFO("[cli] Using provided Vulkan/NGX runtime");
        RpcDispatcher dispatcher;
        std::mutex stopMutex;
        std::function<void()> stopCurrentPipeline;

        auto requestStopIfNeeded = [&] {
            if (!options.cancelRequested || !options.cancelRequested()) {
                return;
            }
            std::lock_guard<std::mutex> lock(stopMutex);
            if (stopCurrentPipeline) {
                stopCurrentPipeline();
            }
        };

        auto runPipeline = [&] {
        if (options.fgEnabled && needsSr(config)) {
            SrFgConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.encoderOptions = encoderOptions;
            pipelineConfig.writerThreads = writerThreads;
            pipelineConfig.writerQueueMultiplier = writerQueueMultiplier;
            pipelineConfig.presetOverride = presetOverride;
            pipelineConfig.h265Options = options.h265Options;
            pipelineConfig.previewCallback = previewCallback;
            pipelineConfig.previewRgba8Callback = options.previewRgba8Callback;
            pipelineConfig.wantsPreviewFrame = options.wantsPreviewFrame;
            pipelineConfig.fgMultiplier = options.fgMultiplier;
            SrFgPipeline pipeline(&vulkan, &ngx, &dispatcher);
            {
                std::lock_guard<std::mutex> lock(stopMutex);
                stopCurrentPipeline = [&pipeline] { pipeline.stop(); };
            }
            pipeline.run(pipelineConfig);
        } else if (options.fgEnabled) {
            FgOnlyConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.encoderOptions = encoderOptions;
            pipelineConfig.writerThreads = writerThreads;
            pipelineConfig.writerQueueMultiplier = writerQueueMultiplier;
            pipelineConfig.h265Options = options.h265Options;
            pipelineConfig.previewCallback = previewCallback;
            pipelineConfig.previewRgba8Callback = options.previewRgba8Callback;
            pipelineConfig.wantsPreviewFrame = options.wantsPreviewFrame;
            pipelineConfig.multiplier = options.fgMultiplier;
            FgOnlyPipeline pipeline(&vulkan, &ngx, &dispatcher);
            {
                std::lock_guard<std::mutex> lock(stopMutex);
                stopCurrentPipeline = [&pipeline] { pipeline.stop(); };
            }
            pipeline.run(pipelineConfig);
        } else {
            SrOnlyConfig pipelineConfig;
            pipelineConfig.dlssConfig = config;
            pipelineConfig.outputFormat = format;
            pipelineConfig.inputDir = options.configPath.parent_path().string();
            pipelineConfig.outputDir = outputDir.string();
            pipelineConfig.filenameTemplate = filenameTemplate;
            pipelineConfig.overwriteMode = overwrite;
            pipelineConfig.encoderOptions = encoderOptions;
            pipelineConfig.writerThreads = writerThreads;
            pipelineConfig.writerQueueMultiplier = writerQueueMultiplier;
            pipelineConfig.presetOverride = presetOverride;
            pipelineConfig.h265Options = options.h265Options;
            pipelineConfig.previewCallback = previewCallback;
            pipelineConfig.previewRgba8Callback = options.previewRgba8Callback;
            pipelineConfig.wantsPreviewFrame = options.wantsPreviewFrame;
            SrOnlyPipeline pipeline(&vulkan, &ngx, &dispatcher);
            {
                std::lock_guard<std::mutex> lock(stopMutex);
                stopCurrentPipeline = [&pipeline] { pipeline.stop(); };
            }
            pipeline.run(pipelineConfig);
        }
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopCurrentPipeline = nullptr;
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
            int lastDlssFrame = -1;
            while (!done.load()) {
                requestStopIfNeeded();
                const int current = dispatcher.progress().current_frame.load();
                const int total = dispatcher.progress().total_frames.load();
                const int dlssCurrent = dispatcher.progress().dlss_current_frame.load();
                const int dlssTotal = dispatcher.progress().dlss_total_frames.load();
                if (total > 0 && (current != lastFrame || dlssCurrent != lastDlssFrame)) {
                    if (options.progressCallback) {
                        options.progressCallback(current, total, dlssCurrent, dlssTotal);
                    }
                    emitProgressEvent(current, total);
                    lastFrame = current;
                    lastDlssFrame = dlssCurrent;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            worker.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            const int current = dispatcher.progress().current_frame.load();
            const int total = dispatcher.progress().total_frames.load();
            const int dlssCurrent = dispatcher.progress().dlss_current_frame.load();
            const int dlssTotal = dispatcher.progress().dlss_total_frames.load();
            if (total > 0 && (current != lastFrame || dlssCurrent != lastDlssFrame)) {
                if (options.progressCallback) {
                    options.progressCallback(current, total, dlssCurrent, dlssTotal);
                }
                emitProgressEvent(current, total);
            }
        } else {
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
            int lastDlssFrame = -1;
            while (!done.load()) {
                requestStopIfNeeded();
                const int current = dispatcher.progress().current_frame.load();
                const int total = dispatcher.progress().total_frames.load();
                const int dlssCurrent = dispatcher.progress().dlss_current_frame.load();
                const int dlssTotal = dispatcher.progress().dlss_total_frames.load();
                if (total > 0 && (current != lastFrame || dlssCurrent != lastDlssFrame)) {
                    if (options.progressCallback) {
                        options.progressCallback(current, total, dlssCurrent, dlssTotal);
                    }
                    lastFrame = current;
                    lastDlssFrame = dlssCurrent;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            worker.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            const int current = dispatcher.progress().current_frame.load();
            const int total = dispatcher.progress().total_frames.load();
            const int dlssCurrent = dispatcher.progress().dlss_current_frame.load();
            const int dlssTotal = dispatcher.progress().dlss_total_frames.load();
            if (total > 0 && (current != lastFrame || dlssCurrent != lastDlssFrame) && options.progressCallback) {
                options.progressCallback(current, total, dlssCurrent, dlssTotal);
            }
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

int executeConfig(const ConfigExecutionOptions& options) {
    try {
        LOG_INFO("[cli] Creating Vulkan context");
        VulkanContext vulkan;
        LOG_INFO("[cli] Vulkan context ready");
        LOG_INFO("[cli] Creating NGX context");
        NgxContext ngx(vulkan.instance(), vulkan.physicalDevice(), vulkan.device());
        LOG_INFO("[cli] NGX context ready");
        return executeConfig(options, vulkan, ngx);
    } catch (const std::exception& error) {
        LOG_ERROR("[cli] Runtime initialization failed: {}", error.what());
        return 4;
    }
}

} // namespace dlss
