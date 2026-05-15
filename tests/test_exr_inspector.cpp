#include <catch2/catch_test_macros.hpp>

#include "inspection/exr_inspector.h"

#include <filesystem>
#include <string>

namespace {
const char* FIXTURE_REFERENCE = "tests/fixtures/reference_64x64.exr";
const char* FIXTURE_SEQUENCE  = "tests/fixtures/sequence/frame_0001.exr";
}

TEST_CASE("ExrInspector inspects existing EXR fixture", "[ExrInspector]") {
    if (!std::filesystem::exists(FIXTURE_REFERENCE)) {
        SKIP("Fixture not found: " + std::string(FIXTURE_REFERENCE));
    }
    ExrMetadata meta;
    std::string err;
    REQUIRE(inspectExr(FIXTURE_REFERENCE, meta, err));
    CHECK(meta.width == 64);
    CHECK(meta.height == 64);
    CHECK(!meta.channels.empty());
    CHECK(!meta.compression.empty());
}

TEST_CASE("ExrInspector handles missing file", "[ExrInspector]") {
    ExrMetadata meta;
    std::string err;
    REQUIRE_FALSE(inspectExr("nonexistent_file_xyz.exr", meta, err));
    CHECK(!err.empty());
}

TEST_CASE("ExrInspector toInspectJson produces valid JSON", "[ExrInspector]") {
    ExrMetadata meta;
    meta.width = 1920;
    meta.height = 1080;
    meta.channels = {"R", "G", "B", "A"};
    meta.compression = "ZIP";
    meta.isMultiPart = false;

    const std::string json = toInspectJson(meta);
    CHECK(json.find("\"width\":1920") != std::string::npos);
    CHECK(json.find("\"height\":1080") != std::string::npos);
    CHECK(json.find("\"R\"") != std::string::npos);
    CHECK(json.find("\"compression\":\"ZIP\"") != std::string::npos);
    CHECK(json.find("\"is_multi_part\":false") != std::string::npos);
}

TEST_CASE("ExrInspector inspects sequence fixture if available", "[ExrInspector]") {
    if (!std::filesystem::exists(FIXTURE_SEQUENCE)) {
        SKIP("Sequence fixture not found");
    }
    ExrMetadata meta;
    std::string err;
    REQUIRE(inspectExr(FIXTURE_SEQUENCE, meta, err));
    CHECK(meta.width > 0);
    CHECK(meta.height > 0);
    CHECK(!meta.channels.empty());
}
