#pragma once

#include "core/channel_mapper.h"
#include "core/exr_reader.h"
#include "core/mv_converter.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct SequenceFrameInfo;

struct PrefetchedFrame {
    std::string path;
    int frameNumber = 0;
    MappedBuffers mappedBuffers;
    MvConvertResult mvResult;
    bool valid = false;
};

class FramePrefetcher {
public:
    FramePrefetcher(const ChannelMapper& mapper, const MvConverter& converter,
                    int ringSize, int expectedWidth, int expectedHeight);
    ~FramePrefetcher();

    FramePrefetcher(const FramePrefetcher&) = delete;
    FramePrefetcher& operator=(const FramePrefetcher&) = delete;

    void start(const std::vector<SequenceFrameInfo>& frames);
    std::optional<PrefetchedFrame> getNext();
    void stop();

private:
    void workerLoop();

    const ChannelMapper& m_mapper;
    const MvConverter& m_converter;
    int m_ringSize;
    int m_expectedWidth;
    int m_expectedHeight;

    std::vector<std::string> m_framePaths;
    std::vector<int> m_frameNumbers;
    std::vector<PrefetchedFrame> m_ring;
    int m_produceCount = 0;
    int m_consumeCount = 0;
    int m_nextFrameToLoad = 0;

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cvProduced;
    std::condition_variable m_cvConsumed;
    std::atomic<bool> m_stopRequested{false};
    bool m_started = false;
};
