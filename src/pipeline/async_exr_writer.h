#pragma once

#include "core/exr_writer.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

/// Asynchronous EXR writer backed by a thread pool.
/// Accepts pixel data via move semantics and writes in background threads,
/// allowing the GPU pipeline to continue without waiting for disk I/O.
class AsyncExrWriter {
public:
    /// @param numThreads  Number of writer threads (0 = auto, capped at 8).
    explicit AsyncExrWriter(unsigned numThreads = 0);
    ~AsyncExrWriter();

    AsyncExrWriter(const AsyncExrWriter&) = delete;
    AsyncExrWriter& operator=(const AsyncExrWriter&) = delete;

    struct WriteJob {
        std::string path;
        int width = 0;
        int height = 0;
        std::vector<float> rgba;          // interleaved RGBA, moved in
        ExrCompression compression = ExrCompression::Dwaa;
        float dwaQuality = 95.0f;
    };

    /// Submit a write job.  Takes ownership of job.rgba via move.
    void submit(WriteJob&& job);

    /// Block until all queued writes have completed.
    void flush();

    /// Number of failed writes since construction.
    int errorCount() const { return m_errorCount.load(std::memory_order_relaxed); }

    /// Total bytes written (rough, based on uncompressed pixel data size).
    size_t pendingCount() const;

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::queue<WriteJob> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;       // signals workers: new job or shutdown
    std::condition_variable m_flushCv;  // signals flush: a job completed
    std::atomic<bool> m_shutdown{false};
    std::atomic<int> m_errorCount{0};
    int m_pendingJobs = 0;              // guarded by m_mutex
};
