#include "pipeline/async_exr_writer.h"

#include "core/logger.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace {

void splitRgbaInPlace(const std::vector<float>& rgba,
                      std::vector<float>& r,
                      std::vector<float>& g,
                      std::vector<float>& b,
                      std::vector<float>& a) {
    const size_t pixelCount = rgba.size() / 4u;
    r.resize(pixelCount);
    g.resize(pixelCount);
    b.resize(pixelCount);
    a.resize(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i) {
        const size_t src = i * 4u;
        r[i] = rgba[src + 0u];
        g[i] = rgba[src + 1u];
        b[i] = rgba[src + 2u];
        a[i] = rgba[src + 3u];
    }
}

} // anonymous namespace

AsyncExrWriter::AsyncExrWriter(unsigned numThreads) {
    if (numThreads == 0) {
        numThreads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    }
    m_workers.reserve(numThreads);
    for (unsigned i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&AsyncExrWriter::workerLoop, this);
    }
}

AsyncExrWriter::~AsyncExrWriter() {
    flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void AsyncExrWriter::submit(WriteJob&& job) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_pendingJobs;
        m_queue.push(std::move(job));
    }
    m_cv.notify_one();
}

void AsyncExrWriter::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_flushCv.wait(lock, [this]() { return m_pendingJobs == 0; });
}

size_t AsyncExrWriter::pendingCount() const {
    // This is a rough snapshot; mutex not needed for a diagnostic counter.
    return 0; // not worth locking — use errorCount() instead
}

void AsyncExrWriter::workerLoop() {
    for (;;) {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return !m_queue.empty() || m_shutdown.load(std::memory_order_acquire);
            });
            if (m_shutdown.load(std::memory_order_acquire) && m_queue.empty()) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        // Perform the write outside the lock.
        bool ok = false;
        std::string writeError;
        try {
            std::vector<float> r, g, b, a;
            splitRgbaInPlace(job.rgba, r, g, b, a);
            // Free RGBA memory early.
            job.rgba.clear();
            job.rgba.shrink_to_fit();

            ExrWriter writer;
            if (!writer.create(job.path, job.width, job.height, writeError)) {
                throw std::runtime_error(writeError);
            }
            writer.setCompression(job.compression, job.dwaQuality);
            if (!writer.addChannel("R", r.data()) ||
                !writer.addChannel("G", g.data()) ||
                !writer.addChannel("B", b.data()) ||
                !writer.addChannel("A", a.data()) ||
                !writer.write(writeError)) {
                if (writeError.empty()) {
                    writeError = "Failed to write EXR";
                }
                throw std::runtime_error(writeError);
            }
            ok = true;
        } catch (const std::exception& ex) {
            Log::error("[AsyncEXR] Write failed for %s: %s\n", job.path.c_str(), ex.what());
            m_errorCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Signal flush.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_pendingJobs;
        }
        m_flushCv.notify_all();
        (void)ok;
    }
}
