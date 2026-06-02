#pragma once
#include "frame_prefetcher.h"
#include <vector>
#include <cstdint>
#include <expected>

namespace dlss {

/// Result of mapping a DecodedFrame to GPU-ready byte buffers.
struct MappedBuffers {
    /// R16G16B16A16_SFLOAT — scene-linear, fp32→fp16 packed
    /// Size: width × height × 4 × 2 bytes
    std::vector<uint16_t> sceneColor;

    /// R32_SFLOAT — Blender world-linear depth (NOT NDC; T22 converts to NDC on GPU)
    /// Size: width × height × 4 bytes (single float per pixel)
    std::vector<float> depth;

    /// R16G16B16A16_SFLOAT — all 4 channels of Blender Vector pass (WXYZ)
    /// Size: width × height × 4 × 2 bytes
    /// T23 shader will select X,Y and apply sign correction (DLSS_MV = Blender.X, -Blender.Y)
    std::vector<uint16_t> motionVector;

    uint32_t width  = 0;
    uint32_t height = 0;
};

enum class MapErrorCode {
    MissingDepthChannel,
    MissingColorChannel,
    InvalidDimensions
};

struct MapError {
    MapErrorCode code;
    std::string message;
};

using MapResult = std::expected<MappedBuffers, MapError>;

/// Maps a CPU-decoded EXR frame to GPU-ready packed byte buffers.
/// Does NOT upload to GPU — caller uses UploadBatch for that.
/// Does NOT do NDC conversion (T22 shader) or MV sign correction (T23 shader).
class ChannelMapper {
public:
    static MapResult map(const DecodedFrame& frame);

private:
    // fp32 to fp16 conversion (IEEE 754 half-float)
    static uint16_t f32ToF16(float v);
};

} // namespace dlss
