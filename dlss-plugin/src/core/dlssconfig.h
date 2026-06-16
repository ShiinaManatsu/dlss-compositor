#pragma once

#include "types.h"

#include <expected>
#include <filesystem>
#include <string>

namespace dlss {

enum class LoadErrorCode {
    BadJson,
    SchemaFail,
    VersionMismatch,
    ResolutionInconsistent,
    FrameKeyMismatch,
    FileNotFound
};

struct LoadError {
    LoadErrorCode code;
    std::string message;
    std::string path;
};

class DlssConfigLoader {
public:
    // Returns loaded config or error. Never throws.
    // Jitter out-of-range: silently clamped to [-0.499, +0.499], LOG_WARN if |x|>0.5
    static std::expected<DlssConfig, LoadError> Load(std::filesystem::path filePath);
};

} // namespace dlss
