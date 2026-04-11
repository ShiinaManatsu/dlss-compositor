#include <catch2/catch_test_macros.hpp>
#include "pipeline/frame_prefetcher.h"
#include "pipeline/sequence_processor.h"
#include "core/channel_mapper.h"
#include "core/mv_converter.h"
#include <filesystem>
#include <string>
#include <vector>

TEST_CASE("frame_prefetcher_basic_sequence", "[pipeline][prefetcher]") {
    ChannelMapper mapper;
    MvConverter converter;

    std::vector<SequenceFrameInfo> frames;
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0001.exr"), 1});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0002.exr"), 2});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0003.exr"), 3});

    FramePrefetcher prefetcher(mapper, converter, 2, 64, 64);
    prefetcher.start(frames);

    const auto first = prefetcher.getNext();
    REQUIRE(first.has_value());
    REQUIRE(first->valid);
    REQUIRE(first->frameNumber == 1);
    REQUIRE(!first->mappedBuffers.color.empty());

    const auto second = prefetcher.getNext();
    REQUIRE(second.has_value());
    REQUIRE(second->valid);
    REQUIRE(second->frameNumber == 2);
    REQUIRE(!second->mappedBuffers.color.empty());

    const auto third = prefetcher.getNext();
    REQUIRE(third.has_value());
    REQUIRE(third->valid);
    REQUIRE(third->frameNumber == 3);
    REQUIRE(!third->mappedBuffers.color.empty());

    REQUIRE(!prefetcher.getNext().has_value());
}

TEST_CASE("frame_prefetcher_empty_frame_list", "[pipeline][prefetcher]") {
    ChannelMapper mapper;
    MvConverter converter;

    std::vector<SequenceFrameInfo> frames;

    FramePrefetcher prefetcher(mapper, converter, 2, 64, 64);
    prefetcher.start(frames);

    REQUIRE(!prefetcher.getNext().has_value());
}

TEST_CASE("frame_prefetcher_stop_before_consume", "[pipeline][prefetcher]") {
    ChannelMapper mapper;
    MvConverter converter;

    std::vector<SequenceFrameInfo> frames;
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0001.exr"), 1});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0002.exr"), 2});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0003.exr"), 3});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0004.exr"), 4});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0005.exr"), 5});

    FramePrefetcher prefetcher(mapper, converter, 1, 64, 64);
    prefetcher.start(frames);

    const auto first = prefetcher.getNext();
    REQUIRE(first.has_value());
    REQUIRE(first->valid);
    REQUIRE(first->frameNumber == 1);

    const auto second = prefetcher.getNext();
    REQUIRE(second.has_value());
    REQUIRE(second->valid);
    REQUIRE(second->frameNumber == 2);

    prefetcher.stop();
    REQUIRE(!prefetcher.getNext().has_value());
}

TEST_CASE("frame_prefetcher_invalid_exr_path", "[pipeline][prefetcher]") {
    ChannelMapper mapper;
    MvConverter converter;

    std::vector<SequenceFrameInfo> frames;
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0001.exr"), 1});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/nonexistent.exr"), 2});
    frames.push_back({std::filesystem::path("tests/fixtures/sequence/frame_0003.exr"), 3});

    FramePrefetcher prefetcher(mapper, converter, 2, 64, 64);
    prefetcher.start(frames);

    const auto first = prefetcher.getNext();
    REQUIRE(first.has_value());
    REQUIRE(first->frameNumber == 1);
    REQUIRE(first->valid);

    const auto second = prefetcher.getNext();
    REQUIRE(second.has_value());
    REQUIRE(second->frameNumber == 2);
    REQUIRE(!second->valid);

    const auto third = prefetcher.getNext();
    REQUIRE(third.has_value());
    REQUIRE(third->frameNumber == 3);
    REQUIRE(third->valid);

    REQUIRE(!prefetcher.getNext().has_value());
}
