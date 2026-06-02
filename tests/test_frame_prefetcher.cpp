// =============================================================================
// FramePrefetcher tests
//
// Exercises the four QA cases from T20:
//   * happy:    a sequence of valid EXRs decodes to non-empty DecodedFrames
//   * missing:  a non-existent path produces ReadErrorCode::FileNotFound
//   * gap:      [0001, 0002, 0005] -> frame #2 has has_gap_after = true
//   * corrupt:  a junk file produces ReadErrorCode::ParseFailed without crash
//
// Real .exr fixtures are not committed to the repo — instead we generate them
// at runtime via OpenEXR's OutputFile so the tests are hermetic.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "../src/io/frame_prefetcher.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
namespace {

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        std::mt19937       rng(rd());
        m_dir = fs::temp_directory_path() /
                ("dlss_pf_" + std::to_string(rng()));
        fs::create_directories(m_dir);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return m_dir; }

private:
    fs::path m_dir;
};

// Write a minimal but valid EXR with R,G,B,A,Z and (optionally) Vector.X/Y/Z/W.
void writeFixtureExr(const fs::path& filePath,
                     int             width,
                     int             height,
                     bool            includeVector = true)
{
    Imf::Header header(width, height);
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    header.channels().insert("Z", Imf::Channel(Imf::FLOAT));
    if (includeVector) {
        header.channels().insert("Vector.X", Imf::Channel(Imf::FLOAT));
        header.channels().insert("Vector.Y", Imf::Channel(Imf::FLOAT));
        header.channels().insert("Vector.Z", Imf::Channel(Imf::FLOAT));
        header.channels().insert("Vector.W", Imf::Channel(Imf::FLOAT));
    }

    const std::size_t pixels = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height);
    std::vector<float> rgba(pixels * 4, 0.5f);
    std::vector<float> z(pixels,        1.0f);
    std::vector<float> vec(pixels * 4,  0.25f);

    Imf::FrameBuffer fb;
    const std::size_t pix_rgba = 4 * sizeof(float);
    const std::size_t row_rgba = pix_rgba * static_cast<std::size_t>(width);
    const std::size_t pix_z    = sizeof(float);
    const std::size_t row_z    = pix_z * static_cast<std::size_t>(width);

    const char* const rgba_names[4] = {"R", "G", "B", "A"};
    for (int ch = 0; ch < 4; ++ch) {
        fb.insert(rgba_names[ch],
                  Imf::Slice(Imf::FLOAT,
                             reinterpret_cast<char*>(rgba.data() + ch),
                             pix_rgba, row_rgba));
    }
    fb.insert("Z",
              Imf::Slice(Imf::FLOAT,
                         reinterpret_cast<char*>(z.data()),
                         pix_z, row_z));
    if (includeVector) {
        const char* const vec_names[4] = {
            "Vector.X", "Vector.Y", "Vector.Z", "Vector.W"};
        for (int ch = 0; ch < 4; ++ch) {
            fb.insert(vec_names[ch],
                      Imf::Slice(Imf::FLOAT,
                                 reinterpret_cast<char*>(vec.data() + ch),
                                 pix_rgba, row_rgba));
        }
    }

    Imf::OutputFile out(filePath.string().c_str(), header);
    out.setFrameBuffer(fb);
    out.writePixels(height);
}

// Filename "<index:04d>.exr"
std::string indexFilename(int idx)
{
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%04d.exr", idx);
    return buf;
}

} // namespace

// ===========================================================================
// FileNotFound — non-existent path
// ===========================================================================
TEST_CASE("FramePrefetcher: missing file -> FileNotFound", "[unit][io]")
{
    fs::path nonExistent = fs::temp_directory_path() / "no_such_frame_0001.exr";
    std::error_code ec;
    fs::remove(nonExistent, ec);

    dlss::FramePrefetcher pf({nonExistent}, 1);

    auto result = pf.getNext();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dlss::ReadErrorCode::FileNotFound);
    REQUIRE(result.error().path == nonExistent);
}

// ===========================================================================
// Corrupt EXR — junk bytes -> ParseFailed (worker stays alive)
// ===========================================================================
TEST_CASE("FramePrefetcher: corrupt EXR -> ParseFailed without crash", "[unit][io]")
{
    TempDir tmp;
    fs::path bad = tmp.path() / "0001.exr";
    {
        std::ofstream f(bad.string(), std::ios::binary);
        // Wrong magic bytes — OpenEXR will throw on InputFile ctor.
        const char junk[] = "not an exr file at all";
        f.write(junk, sizeof(junk) - 1);
    }

    dlss::FramePrefetcher pf({bad}, 1);
    auto result = pf.getNext();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dlss::ReadErrorCode::ParseFailed);
    REQUIRE(pf.isDone());
}

// ===========================================================================
// prefetchDepth clamp — 0 must be coerced to 1 (no infinite-prefetch)
// ===========================================================================
TEST_CASE("FramePrefetcher: prefetchDepth clamps to 1", "[unit][io]")
{
    TempDir tmp;
    fs::path missing = tmp.path() / "0001.exr"; // intentionally not created
    REQUIRE_NOTHROW([&] {
        dlss::FramePrefetcher pf({missing}, 0);
        auto r = pf.getNext();
        REQUIRE_FALSE(r.has_value());
    }());
}

// ===========================================================================
// Happy path — 5 valid EXRs all decode with width/height set
// ===========================================================================
TEST_CASE("FramePrefetcher: 5 valid EXRs all decode", "[unit][io]")
{
    TempDir tmp;
    constexpr int kWidth  = 32;
    constexpr int kHeight = 16;
    constexpr int kFrames = 5;

    std::vector<fs::path> paths;
    paths.reserve(kFrames);
    for (int i = 1; i <= kFrames; ++i) {
        fs::path p = tmp.path() / indexFilename(i);
        writeFixtureExr(p, kWidth, kHeight, /*includeVector=*/true);
        paths.push_back(p);
    }

    dlss::FramePrefetcher pf(paths, 3);
    int success = 0;
    for (int i = 0; i < kFrames; ++i) {
        auto r = pf.getNext();
        REQUIRE(r.has_value());
        REQUIRE(r->width  == static_cast<std::uint32_t>(kWidth));
        REQUIRE(r->height == static_cast<std::uint32_t>(kHeight));
        REQUIRE(r->combined.size() == static_cast<std::size_t>(kWidth * kHeight * 4));
        REQUIRE(r->depth.size()    == static_cast<std::size_t>(kWidth * kHeight));
        REQUIRE(r->vector.size()   == static_cast<std::size_t>(kWidth * kHeight * 4));
        // No gaps in [1..5].
        REQUIRE_FALSE(r->has_gap_after);
        ++success;
    }
    REQUIRE(success == kFrames);
    REQUIRE(pf.isDone());
}

// ===========================================================================
// Gap detection — [0001, 0002, 0005] -> frame #2 has has_gap_after = true
// ===========================================================================
TEST_CASE("FramePrefetcher: detects non-sequential frame indices", "[unit][io]")
{
    TempDir tmp;
    constexpr int kWidth  = 16;
    constexpr int kHeight = 8;

    const int indices[] = {1, 2, 5};
    std::vector<fs::path> paths;
    for (int idx : indices) {
        fs::path p = tmp.path() / indexFilename(idx);
        writeFixtureExr(p, kWidth, kHeight, /*includeVector=*/false);
        paths.push_back(p);
    }

    dlss::FramePrefetcher pf(paths, 3);

    auto f1 = pf.getNext();
    auto f2 = pf.getNext();
    auto f3 = pf.getNext();

    REQUIRE(f1.has_value());
    REQUIRE(f2.has_value());
    REQUIRE(f3.has_value());

    REQUIRE_FALSE(f1->has_gap_after); // 1 -> 2 sequential
    REQUIRE      (f2->has_gap_after); // 2 -> 5 GAP
    REQUIRE_FALSE(f3->has_gap_after); // last frame, never flagged
}

// ===========================================================================
// Optional fixture sweep (only runs if tests/fixtures/sequence/ exists)
// ===========================================================================
TEST_CASE("FramePrefetcher: tests/fixtures/sequence sweep", "[gpu][io]")
{
    fs::path fixturesDir = "tests/fixtures/sequence";
    if (!fs::exists(fixturesDir)) {
        SKIP("tests/fixtures/sequence directory not present");
    }

    std::vector<fs::path> exrFiles;
    for (auto& entry : fs::directory_iterator(fixturesDir)) {
        if (entry.path().extension() == ".exr") {
            exrFiles.push_back(entry.path());
        }
    }
    std::sort(exrFiles.begin(), exrFiles.end());

    if (exrFiles.size() < 2) {
        SKIP("Need at least 2 EXR fixtures");
    }

    dlss::FramePrefetcher pf(exrFiles, 3);
    int successCount = 0;
    int errorCount   = 0;
    for (std::size_t i = 0; i < exrFiles.size(); ++i) {
        auto result = pf.getNext();
        if (result.has_value()) {
            ++successCount;
            REQUIRE(result->width  > 0);
            REQUIRE(result->height > 0);
        } else {
            ++errorCount;
        }
    }
    INFO("Read " << successCount << " frames, " << errorCount << " errors");
    REQUIRE(successCount > 0);
}
