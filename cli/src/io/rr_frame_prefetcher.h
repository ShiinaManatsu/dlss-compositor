#pragma once
// RrFramePrefetcher — async reader for Blender 5.3 RR multilayer EXR frames.
//
// Parallel to FramePrefetcher (SR), but decodes the additional RR guide
// channels from each multilayer EXR written by inject.py in --mode rr.
//
// Expected channel names (case-sensitive, Blender 5.3 inject.py output):
//   Image.R/G/B/A
//   Denoising Depth.V
//   Vector.X/Y/Z/W
//   Denoising Albedo.R/G/B/A
//   Denoising Specular Albedo.R/G/B/A
//   Denoising Normal.X/Y/Z/W
//   Denoising Roughness.V
//   Denoising Specular Motion.X/Y/Z/W   (Blender 5.3+, optional)
//   Denoising Backward Motion.X/Y/Z/W   (Blender 5.3+, decoded but not fed to RR pass)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dlss {

// Reuse ReadError / ReadErrorCode from the SR prefetcher.
enum class RrReadErrorCode {
    FileNotFound,
    ParseFailed,
    MissingMandatoryChannel,
    IoError
};

struct RrReadError {
    RrReadErrorCode       code;
    std::string           message;
    std::filesystem::path path;
};

/// All channels decoded from one Blender 5.3 RR EXR frame.
/// Float32 throughout — RrChannelMapper converts to fp16/R32 for GPU upload.
struct RrDecodedFrame {
    // ── Core ─────────────────────────────────────────────────────────────
    std::vector<float> colorRgba;         ///< "Image" RGBA     — size = W*H*4
    std::vector<float> depthLinear;       ///< "Denoising Depth.V" — size = W*H
    std::vector<float> motionXy;          ///< "Vector" X/Y     — size = W*H*2 (Blender space)

    // ── RR guide buffers (mandatory in RR mode) ──────────────────────────
    std::vector<float> diffuseAlbedoRgba; ///< "Denoising Albedo" RGBA        — size = W*H*4
    std::vector<float> specAlbedoRgba;    ///< "Denoising Specular Albedo" RGBA — size = W*H*4
    std::vector<float> normalsXyz;        ///< "Denoising Normal" XYZ          — size = W*H*3
    std::vector<float> roughness;         ///< "Denoising Roughness.V"          — size = W*H

    // ── Blender 5.3+ optional passes ─────────────────────────────────────
    std::vector<float> specularMotionXy;  ///< "Denoising Specular Motion" X/Y — size = W*H*2 (Blender space)
    std::vector<float> backwardMotionXy;  ///< "Denoising Backward Motion" X/Y — size = W*H*2 (for future use)

    uint32_t width             = 0;
    uint32_t height            = 0;
    bool     hasSpecularMotion = false; ///< specularMotionXy populated
    bool     hasBackwardMotion = false;
    bool     has_gap_after     = false;

    std::filesystem::path source_path;
};

using RrFrameResult = std::expected<RrDecodedFrame, RrReadError>;

/// Async EXR reader for Blender 5.3 RR multilayer frames.
/// Same prefetch-queue pattern as FramePrefetcher.
class RrFramePrefetcher {
public:
    /// @param framePaths    Ordered EXR paths (all frames in the run).
    /// @param prefetchDepth Frames to read ahead (clamped >= 1).
    /// @param waitForFiles  Watch mode: block each decode until the file exists
    ///                      and has a stable size (Blender is still writing it).
    /// @param externalStop  Optional pipeline stop flag; all waits also wake when
    ///                      it flips so stop() can cancel mid-wait (e.g. Blender
    ///                      was killed and the next frame will never appear).
    explicit RrFramePrefetcher(std::vector<std::filesystem::path> framePaths,
                               int prefetchDepth = 3,
                               bool waitForFiles = false,
                               const std::atomic<bool>* externalStop = nullptr);

    ~RrFramePrefetcher();

    RrFramePrefetcher(const RrFramePrefetcher&)            = delete;
    RrFramePrefetcher& operator=(const RrFramePrefetcher&) = delete;
    RrFramePrefetcher(RrFramePrefetcher&&)                 = delete;
    RrFramePrefetcher& operator=(RrFramePrefetcher&&)      = delete;

    /// Decode a single RR multilayer EXR (blocking). Used by the FileWatcher input path.
    static RrFrameResult decodeFile(const std::filesystem::path& path);

    /// Blocking — returns next decoded RR frame or RrReadError.
    RrFrameResult getNext();

    bool isDone() const noexcept {
        return m_consumed.load() >= static_cast<int>(m_paths.size());
    }
    int totalFrames() const noexcept {
        return static_cast<int>(m_paths.size());
    }

private:
    void workerLoop();
    void waitUntilWritten(const std::filesystem::path& path) const;
    bool stopRequested() const noexcept {
        return m_stop.load() || (m_externalStop && m_externalStop->load());
    }

    std::vector<std::filesystem::path> m_paths;
    int                                m_prefetchDepth;
    bool                               m_waitForFiles;
    const std::atomic<bool>*           m_externalStop;

    mutable std::mutex        m_mutex;
    std::condition_variable   m_cv;
    std::queue<RrFrameResult> m_queue;
    std::atomic<bool>         m_stop{false};
    std::atomic<int>          m_produced{0};
    std::atomic<int>          m_consumed{0};
    std::thread               m_worker;
};

} // namespace dlss
