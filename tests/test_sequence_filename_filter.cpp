#include <catch2/catch_test_macros.hpp>

#include "pipeline/sequence_processor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TempSequenceDir {
public:
    TempSequenceDir() {
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        m_path = std::filesystem::temp_directory_path() / ("sequence_filename_filter-" + unique);
        std::filesystem::create_directories(m_path);
    }

    ~TempSequenceDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    const std::filesystem::path& path() const {
        return m_path;
    }

    void touch(const std::string& name) const {
        std::ofstream out(m_path / name, std::ios::binary);
        REQUIRE(out.good());
    }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("SequenceFilenameFilter skips EXR files without trailing frame numbers", "[SequenceFilenameFilter][unit]") {
    TempSequenceDir dir;
    dir.touch("frame_0002.exr");
    dir.touch("preview.exr");
    dir.touch("frame_.exr");
    dir.touch("frame_0001.exr");

    std::vector<SequenceFrameInfo> frames;
    std::string errorMsg;

    const bool ok = SequenceProcessor::scanAndSort(dir.path(), frames, errorMsg);

    REQUIRE(ok);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].path.filename().string() == "frame_0001.exr");
    CHECK(frames[0].frameNumber == 1);
    CHECK(frames[1].path.filename().string() == "frame_0002.exr");
    CHECK(frames[1].frameNumber == 2);
}

TEST_CASE("SequenceFilenameFilter returns error when all EXR files are invalid", "[SequenceFilenameFilter][unit]") {
    TempSequenceDir dir;
    dir.touch("preview.exr");
    dir.touch("frame_.exr");
    dir.touch("_intermediate.exr");

    std::vector<SequenceFrameInfo> frames;
    std::string errorMsg;

    const bool ok = SequenceProcessor::scanAndSort(dir.path(), frames, errorMsg);

    REQUIRE_FALSE(ok);
    CHECK(frames.empty());
    CHECK(errorMsg == "No EXR files with valid frame numbers found in input directory: " + dir.path().string());
}

TEST_CASE("SequenceFilenameFilter ignores non-EXR files and sorts duplicate frame numbers by filename", "[SequenceFilenameFilter][unit]") {
    TempSequenceDir dir;
    dir.touch("shotA_0010.exr");
    dir.touch("notes.txt");
    dir.touch("shotB_0010.exr");
    dir.touch("shot_0003.exr");

    std::vector<SequenceFrameInfo> frames;
    std::string errorMsg;

    const bool ok = SequenceProcessor::scanAndSort(dir.path(), frames, errorMsg);

    REQUIRE(ok);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].path.filename().string() == "shot_0003.exr");
    CHECK(frames[0].frameNumber == 3);
    CHECK(frames[1].path.filename().string() == "shotA_0010.exr");
    CHECK(frames[1].frameNumber == 10);
    CHECK(frames[2].path.filename().string() == "shotB_0010.exr");
    CHECK(frames[2].frameNumber == 10);
}
