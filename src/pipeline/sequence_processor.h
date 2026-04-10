#pragma once

#include <filesystem>
#include <string>
#include <vector>

class VulkanContext;
class NgxContext;
class TexturePipeline;
struct AppConfig;

struct SequenceFrameInfo {
    std::filesystem::path path;
    int frameNumber = -1;
};

class SequenceProcessor {
public:
    SequenceProcessor(VulkanContext& ctx, NgxContext& ngx, TexturePipeline& texturePipeline);

    bool processDirectory(const std::string& inputDir,
                          const std::string& outputDir,
                          const AppConfig& config,
                          std::string& errorMsg);

    static bool scanAndSort(const std::filesystem::path& inputDir,
                            std::vector<SequenceFrameInfo>& frames,
                            std::string& errorMsg);
    static std::vector<bool> computeResetFlags(const std::vector<SequenceFrameInfo>& frames);

private:
    VulkanContext& m_ctx;
    NgxContext& m_ngx;
    TexturePipeline& m_texturePipeline;
};
