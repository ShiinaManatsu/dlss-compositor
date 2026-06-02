#pragma once
// =============================================================================
// AsyncFrameWriter — non-blocking, multi-threaded frame writer.
//
// Workflow:
//   1. Caller enqueue()s WriteJob with format + pixels + outputPath.
//   2. enqueue() pushes to internal queue and returns immediately.
//   3. Worker threads pull jobs, look up encoder via EncoderRegistry, write file.
//   4. Caller flush()es to wait for all queued jobs to finish.
//
// Constraints (T29):
//   - HDR + PNG combination is REJECTED at enqueue() time (throws).
//   - Only ExrLinear / Png16 / ProRes422HQ are registered encoders.
//   - Thread count: ctor param > env DLSS_COMPOSITOR_WRITER_THREADS > 4 default.
// =============================================================================

#include "../core/types.h"
#include "encoder_registry.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dlss {

enum class OverwriteMode {
    Skip,        ///< Skip existing files + LOG_WARN.
    Overwrite,   ///< Overwrite existing files (default).
    AutoVersion  ///< Append _v01, _v02, ... until a non-existing path is found.
};

struct WriteJob {
    OutputFormat          format       = OutputFormat::ExrLinear;
    std::filesystem::path outputPath;
    std::vector<uint8_t>  pixels;
    uint32_t              width        = 0;
    uint32_t              height       = 0;
    int                   fps          = 24;
    bool                  is_hdr       = false;
    OverwriteMode         overwriteMode = OverwriteMode::Overwrite;
};

enum class WriteErrorCode {
    DiskFull,
    PermissionDenied,
    EncoderFailed,
    HdrPngUnsupported,
    NoEncoder
};

struct WriteError {
    WriteErrorCode        code{};
    std::string           message;
    std::filesystem::path path;
};

/// Async multi-threaded frame writer.
/// Enqueues write jobs and processes them on background threads — does NOT
/// block the caller (enqueue returns immediately).
class AsyncFrameWriter {
public:
    /// numThreads: explicit thread count.  When left at 0 (default), the env
    /// variable DLSS_COMPOSITOR_WRITER_THREADS is consulted; falls back to 4.
    explicit AsyncFrameWriter(int numThreads = 0);
    ~AsyncFrameWriter();

    AsyncFrameWriter(const AsyncFrameWriter&)            = delete;
    AsyncFrameWriter& operator=(const AsyncFrameWriter&) = delete;

    /// Enqueue a frame for async writing. Returns immediately.
    /// Throws std::runtime_error if format is HDR+PNG (unsupported per T29).
    void enqueue(WriteJob job);

    /// Block until all queued jobs are written.
    void flush();

    /// Errors recorded by worker threads (call after flush()).
    bool                    hasErrors() const;
    std::vector<WriteError> getErrors() const;

    /// Pre-scan output directory for collision count.
    /// Returns number of files that would be overwritten.
    static int checkOutputCollisions(
        const std::filesystem::path& outputDir,
        const std::string&           filenameTemplate,
        int                          frameCount,
        OutputFormat                 fmt);

    /// Parse filename template:
    ///   "frame_####.exr"  → "frame_0042.exr"   (#### == 4-digit zero-pad)
    ///   "{frame:04d}.exr" → "0042.exr"
    /// `version` (>0) inserts "_vNN" before the file extension.
    static std::string formatPath(const std::string& tmpl,
                                  int                frameNum,
                                  int                version = 0);

private:
    void                  workerLoop();
    void                  processJob(WriteJob& job);
    std::filesystem::path resolveOutputPath(const WriteJob& job, bool& outShouldSkip);
    void                  recordError(WriteError err);

    int                          m_numThreads;
    std::vector<std::thread>     m_workers;
    mutable std::mutex           m_mutex;
    std::condition_variable      m_cv;
    std::queue<WriteJob>         m_queue;
    std::atomic<bool>            m_stop{false};
    std::atomic<int>             m_pending{0};
    mutable std::mutex           m_errorMutex;
    std::vector<WriteError>      m_errors;
    std::mutex                   m_flushMutex;
    std::condition_variable      m_flushCv;
};

} // namespace dlss
