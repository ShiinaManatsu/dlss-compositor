#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dlss {

class VulkanContext;
class NgxContext;

struct ConfigExecutionOptions {
    std::filesystem::path configPath;
    std::filesystem::path outputDir;
    std::string outputTemplate;
    std::string outputFormat;
    std::string overwriteMode;
    bool fgEnabled = false;
    int fgMultiplier = 2;
    bool emitEvents = false;
    std::string previewSharedMemoryName;
    std::function<void(int currentFrame,
                       int totalFrames,
                       int dlssCurrentFrame,
                       int dlssTotalFrames)> progressCallback;
    std::function<bool()> cancelRequested;
    std::function<bool()> wantsPreviewFrame;
    std::function<void(const std::vector<std::uint8_t>& pixels,
                       std::uint32_t width,
                       std::uint32_t height)> previewRgba8Callback;
    std::function<void(const std::vector<std::uint8_t>& pixels,
                       std::uint32_t width,
                       std::uint32_t height)> previewCallback;
};

int executeConfig(const ConfigExecutionOptions& options);
int executeConfig(const ConfigExecutionOptions& options, VulkanContext& vulkan, NgxContext& ngx);

} // namespace dlss
