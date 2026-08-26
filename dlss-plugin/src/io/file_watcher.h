#pragma once
// FileWatcher — monitors a directory for new EXR files from Blender,
// fires a callback, then deletes the file to keep the RAM disk at low watermark.
//
// Windows-only (ReadDirectoryChangesW).
// FileWatcherQueue provides a thread-safe producer-consumer queue for
// decoupling the watcher thread from the pipeline thread.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// HANDLE is void* on Windows; forward-declare to avoid pulling in windows.h here.
// The .cpp includes windows.h directly.
#if !defined(_WINDEF_)
using HANDLE = void*;
#endif

namespace dlss {

struct FileWatcherConfig {
    std::filesystem::path directory;    ///< Directory to watch (must exist)
    std::string           extension = ".exr"; ///< Only notify for files with this suffix
    int stabilityMs = 80;               ///< Wait this long after last write before notifying
    bool keepFiles  = false;            ///< If true, do NOT delete after callback (debug)
    int  bufferSize = 65536;            ///< ReadDirectoryChangesW buffer size (bytes)
};

struct FileEvent {
    std::filesystem::path path;     ///< Full path to the new/completed file
    std::string stem;               ///< Filename without extension (e.g. "0300")
    bool        deleted = false;    ///< True if the file was successfully deleted after callback
};

/// Callbacks from the watcher thread; keep them lightweight or enqueue work.
struct FileWatcherCallbacks {
    /// A new file has appeared, been stabilised, and is ready for consumption.
    /// Called *before* deletion.  Returning normally triggers deletion.
    std::function<void(const FileEvent&)> onFile;

    /// A file matching the extension was deleted from the watched directory
    /// by an external agent (unexpected).
    std::function<void(const std::filesystem::path&)> onExternalDelete;

    /// Non-fatal watcher error (Win32 error message).
    std::function<void(const std::string& msg)> onError;
};

/// Watches a directory for new EXR files and fires callbacks.
/// Construction starts the background thread.  Call stop() to shut down.
class FileWatcher {
public:
    explicit FileWatcher(FileWatcherConfig cfg, FileWatcherCallbacks cbs);
    ~FileWatcher();

    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Signal the watcher to stop.  Returns immediately; the watcher thread
    /// finishes its current callback and then exits.
    void stop();

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    /// Total files delivered to onFile since construction.
    int  deliveredCount() const { return m_delivered.load(std::memory_order_relaxed); }

private:
    void workerLoop();
    bool isTarget(const std::wstring& name) const;
    void waitForStability(const std::filesystem::path& path) const;

    FileWatcherConfig    m_cfg;
    FileWatcherCallbacks m_cbs;
    std::atomic<bool>    m_stop{false};
    std::atomic<bool>    m_running{false};
    std::atomic<int>     m_delivered{0};
    HANDLE               m_dirHandle  = nullptr;  // set to INVALID_HANDLE_VALUE in .cpp ctor
    HANDLE               m_stopEvent  = nullptr;  // set to INVALID_HANDLE_VALUE in .cpp ctor
    std::thread          m_worker;
};

// ─────────────────────────────────────────────────────────────────────────────
// FileWatcherQueue
//
// Thread-safe queue that decouples the watcher thread from the pipeline thread.
// The pipeline calls pop() (blocking) to get the next ready file path.
// Use this when the pipeline and watcher run in different threads (normal case).
// ─────────────────────────────────────────────────────────────────────────────

#include <condition_variable>
#include <mutex>
#include <queue>

class FileWatcherQueue {
public:
    /// Push is called from the FileWatcher callback thread.
    void push(std::filesystem::path p) {
        { std::lock_guard lk(m_mu); m_q.push(std::move(p)); }
        m_cv.notify_one();
    }

    /// Pop blocks until a path is available or stop() has been called.
    /// Returns empty path on stop.
    std::filesystem::path pop() {
        std::unique_lock lk(m_mu);
        m_cv.wait(lk, [&]{ return !m_q.empty() || m_stop.load(); });
        if (m_q.empty()) return {};
        auto p = std::move(m_q.front());
        m_q.pop();
        return p;
    }

    /// Try to pop without blocking.  Returns empty path if nothing available.
    std::filesystem::path tryPop() {
        std::lock_guard lk(m_mu);
        if (m_q.empty()) return {};
        auto p = std::move(m_q.front());
        m_q.pop();
        return p;
    }

    /// Signal the consumer to stop waiting.
    void stop() { m_stop.store(true); m_cv.notify_all(); }

    bool empty() const { std::lock_guard lk(m_mu); return m_q.empty(); }
    int  size()  const { std::lock_guard lk(m_mu); return (int)m_q.size(); }

private:
    mutable std::mutex       m_mu;
    std::condition_variable  m_cv;
    std::queue<std::filesystem::path> m_q;
    std::atomic<bool>        m_stop{false};
};

} // namespace dlss
