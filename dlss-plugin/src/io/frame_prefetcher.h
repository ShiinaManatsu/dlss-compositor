#pragma once
// =============================================================================
// FramePrefetcher — async EXR frame reader for the DLSS pipeline.
//
//   - Background worker thread eagerly reads up to `prefetchDepth` frames ahead
//     while the main thread is busy uploading/decoding the current frame.
//   - Errors propagate via std::expected<DecodedFrame, ReadError> (C++23).
//   - Frame gaps (non-sequential numeric stems) are detected at decode time
//     and reported via DecodedFrame::has_gap_after so DLSS can request a reset.
// =============================================================================

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

enum class ReadErrorCode {
    FileNotFound,
    ParseFailed,
    MissingChannel,
    IoError
};

struct ReadError {
    ReadErrorCode      code;
    std::string        message;
    std::filesystem::path path;
};

struct DecodedFrame {
    std::vector<float> combined;   // RGBA float, size = width*height*4
    std::vector<float> depth;      // R    float, size = width*height
    std::vector<float> vector;     // WXYZ float, size = width*height*4 (zero if absent)
    std::uint32_t      width  = 0;
    std::uint32_t      height = 0;
    bool               has_gap_after = false;
    std::filesystem::path source_path;
};

using FrameResult = std::expected<DecodedFrame, ReadError>;

/// Async EXR frame reader with a configurable prefetch window.
///
/// Worker thread reads ahead while the main thread processes current frames.
/// Construction starts the worker immediately; destruction joins it.
class FramePrefetcher {
public:
    /// @param framePaths    Ordered list of EXR file paths (all frames in run).
    /// @param prefetchDepth How many frames to read ahead (clamped to >= 1).
    explicit FramePrefetcher(std::vector<std::filesystem::path> framePaths,
                             int prefetchDepth = 3);

    ~FramePrefetcher();

    FramePrefetcher(const FramePrefetcher&)            = delete;
    FramePrefetcher& operator=(const FramePrefetcher&) = delete;
    FramePrefetcher(FramePrefetcher&&)                 = delete;
    FramePrefetcher& operator=(FramePrefetcher&&)      = delete;

    /// Blocking — returns next decoded frame or ReadError.
    /// Returns IoError if the prefetcher has been stopped with no frames left.
    FrameResult getNext();

    /// True once all frames have been consumed.
    bool isDone() const noexcept {
        return m_consumed.load() >= static_cast<int>(m_paths.size());
    }

    int totalFrames() const noexcept {
        return static_cast<int>(m_paths.size());
    }

private:
    void workerLoop();

    std::vector<std::filesystem::path> m_paths;
    int                                m_prefetchDepth;

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::queue<FrameResult> m_queue;
    std::atomic<bool>       m_stop{false};
    std::atomic<int>        m_produced{0};
    std::atomic<int>        m_consumed{0};
    std::thread             m_worker;
};

} // namespace dlss
