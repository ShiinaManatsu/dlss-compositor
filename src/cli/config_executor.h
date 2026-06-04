#pragma once

#include <filesystem>
#include <string>

namespace dlss {

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
};

int executeConfig(const ConfigExecutionOptions& options);

} // namespace dlss
