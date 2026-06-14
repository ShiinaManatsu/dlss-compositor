#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace dlss {

struct Lut3D {
    int size = 0;
    float domain_min[3] = {0.0f, 0.0f, 0.0f};
    float domain_max[3] = {1.0f, 1.0f, 1.0f};
    bool input_log2 = false;
    float log2_min = -12.0f;
    float log2_max = 4.0f;
    std::vector<float> rgb;
};

enum class LutLoadErrorCode {
    FileNotFound,
    BadFormat,
    Unsupported,
};

struct LutLoadError {
    LutLoadErrorCode code;
    std::string message;
    std::string path;
};

class LutLoader {
public:
    static std::expected<Lut3D, LutLoadError> LoadCube(const std::filesystem::path& filePath);
};

} // namespace dlss
