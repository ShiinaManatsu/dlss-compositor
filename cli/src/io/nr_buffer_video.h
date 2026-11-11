#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace dlss {

inline constexpr char kNrBufferVideoName[] = "dlssnrbuffer.mkv";
inline constexpr char kNrBufferTrackTitle[] = "DLSSNR guides v1: X / Y / depth (FP16 bits)";
inline constexpr int kNrBufferPlanes = 3;

/// Stack MV.x, MV.y and NDC depth as three gray16 panels. FFV1 preserves
/// their FP16 bits without color conversion, subsampling or frame dependency.
/// Motion is rounded to 1/32 px before encoding; depth retains FP16 precision.
class NrBufferVideoWriter {
public:
    NrBufferVideoWriter(const std::filesystem::path& path, int width, int height, int fps);
    ~NrBufferVideoWriter();

    void write(const std::vector<std::uint8_t>& motion, const std::vector<std::uint8_t>& depth);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace dlss
