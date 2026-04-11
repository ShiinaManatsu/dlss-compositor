#include "pipeline/frame_prefetcher.h"

#include "pipeline/sequence_processor.h"

#include <utility>

FramePrefetcher::FramePrefetcher(const ChannelMapper& mapper, const MvConverter& converter,
                                 int ringSize, int expectedWidth, int expectedHeight)
    : m_mapper(mapper),
      m_converter(converter),
      m_ringSize(ringSize),
      m_expectedWidth(expectedWidth),
      m_expectedHeight(expectedHeight) {
    m_ring.resize(static_cast<size_t>(ringSize));
}

FramePrefetcher::~FramePrefetcher() {
    stop();
}

void FramePrefetcher::start(const std::vector<SequenceFrameInfo>& frames) {
    if (m_started) {
        return;
    }

    m_framePaths.clear();
    m_frameNumbers.clear();
    m_framePaths.reserve(frames.size());
    m_frameNumbers.reserve(frames.size());
    for (const auto& frame : frames) {
        m_framePaths.push_back(frame.path.string());
        m_frameNumbers.push_back(frame.frameNumber);
    }

    m_nextFrameToLoad = 0;
    m_produceCount = 0;
    m_consumeCount = 0;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_started = true;
    m_worker = std::thread(&FramePrefetcher::workerLoop, this);
}

std::optional<PrefetchedFrame> FramePrefetcher::getNext() {
    std::unique_lock<std::mutex> lock(m_mutex);

    const int totalFrames = static_cast<int>(m_framePaths.size());

    m_cvProduced.wait(lock, [this, totalFrames] {
        if (m_stopRequested.load(std::memory_order_relaxed) && m_consumeCount >= m_produceCount) {
            return true;
        }
        if (m_consumeCount >= totalFrames) {
            return true;
        }
        return m_consumeCount < m_produceCount;
    });

    if (m_consumeCount >= totalFrames) {
        return std::nullopt;
    }
    if (m_stopRequested.load(std::memory_order_relaxed) && m_consumeCount >= m_produceCount) {
        return std::nullopt;
    }

    const int ringSlot = m_consumeCount % m_ringSize;
    PrefetchedFrame result = std::move(m_ring[static_cast<size_t>(ringSlot)]);
    m_consumeCount++;
    m_cvConsumed.notify_one();
    return result;
}

void FramePrefetcher::stop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    m_cvConsumed.notify_all();
    m_cvProduced.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_started = false;
}

void FramePrefetcher::workerLoop() {
    const int totalFrames = static_cast<int>(m_framePaths.size());

    while (true) {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cvConsumed.wait(lock, [this, totalFrames] {
            if (m_stopRequested.load(std::memory_order_relaxed)) {
                return true;
            }
            if (m_nextFrameToLoad >= totalFrames) {
                return true;
            }
            return (m_produceCount - m_consumeCount) < m_ringSize;
        });

        if (m_stopRequested.load(std::memory_order_relaxed)) {
            break;
        }
        if (m_nextFrameToLoad >= totalFrames) {
            break;
        }

        const int frameIdx = m_nextFrameToLoad++;
        const std::string path = m_framePaths[static_cast<size_t>(frameIdx)];
        const int frameNum = m_frameNumbers[static_cast<size_t>(frameIdx)];
        const int ringSlot = m_produceCount % m_ringSize;

        lock.unlock();

        PrefetchedFrame prefetchedFrame;
        prefetchedFrame.path = path;
        prefetchedFrame.frameNumber = frameNum;
        try {
            ExrReader reader;
            std::string openError;
            if (reader.open(path, openError) &&
                reader.width() == m_expectedWidth &&
                reader.height() == m_expectedHeight) {
                MappedBuffers mappedBuffers;
                std::string mapError;
                if (m_mapper.mapFromExr(reader, mappedBuffers, mapError)) {
                    prefetchedFrame.mvResult = m_converter.convert(
                        mappedBuffers.motionVectors.data(), m_expectedWidth, m_expectedHeight);
                    prefetchedFrame.mappedBuffers = std::move(mappedBuffers);
                    prefetchedFrame.valid = true;
                }
            }
        } catch (...) {
            prefetchedFrame.valid = false;
        }

        lock.lock();
        m_ring[static_cast<size_t>(ringSlot)] = std::move(prefetchedFrame);
        m_produceCount++;
        m_cvProduced.notify_one();
    }

    m_cvProduced.notify_all();
}
