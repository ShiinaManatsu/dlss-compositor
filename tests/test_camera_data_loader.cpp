#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/camera_data_loader.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

using Catch::Matchers::WithinAbs;

static const char* kValidCameraJson = R"({
    "version": 1,
    "render_width": 1920,
    "render_height": 1080,
    "frames": {
        "0001": {
            "matrix_world": [
                [1.0, 0.0, 0.0, 5.0],
                [0.0, 1.0, 0.0, 3.0],
                [0.0, 0.0, 1.0, -10.0],
                [0.0, 0.0, 0.0, 1.0]
            ],
            "projection": [
                [1.8106, 0.0, 0.0, 0.0],
                [0.0, 3.2189, 0.0, 0.0],
                [0.0, 0.0, -1.002, -0.2002],
                [0.0, 0.0, -1.0, 0.0]
            ],
            "fov": 0.6911,
            "aspect_ratio": 1.7778,
            "near_clip": 0.1,
            "far_clip": 100.0
        },
        "0002": {
            "matrix_world": [
                [0.9659, 0.0, 0.2588, 6.0],
                [0.0, 1.0, 0.0, 3.0],
                [-0.2588, 0.0, 0.9659, -9.0],
                [0.0, 0.0, 0.0, 1.0]
            ],
            "projection": [
                [1.8106, 0.0, 0.0, 0.0],
                [0.0, 3.2189, 0.0, 0.0],
                [0.0, 0.0, -1.002, -0.2002],
                [0.0, 0.0, -1.0, 0.0]
            ],
            "fov": 0.6911,
            "aspect_ratio": 1.7778,
            "near_clip": 0.1,
            "far_clip": 100.0
        }
    }
})";

// Helper to write a temp file and return its path
static std::string writeTempJson(const std::string& content, const std::string& name) {
    std::string path = name;
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// =========================================================================
// Scenario 1: Valid camera JSON parsing
// =========================================================================
TEST_CASE("CameraDataLoader parses valid camera JSON", "[camera]") {
    auto path = writeTempJson(kValidCameraJson, "test_camera_valid.json");
    CameraDataLoader loader;
    std::string err;

    REQUIRE(loader.load(path, err));
    REQUIRE(err.empty());

    SECTION("metadata is correct") {
        CHECK(loader.renderWidth() == 1920);
        CHECK(loader.renderHeight() == 1080);
        CHECK(loader.frameCount() == 2);
    }

    SECTION("hasFrame works with int keys") {
        CHECK(loader.hasFrame(1));
        CHECK(loader.hasFrame(2));
        CHECK_FALSE(loader.hasFrame(0));
        CHECK_FALSE(loader.hasFrame(3));
    }

    SECTION("getFrame returns correct data for frame 1") {
        const auto& f = loader.getFrame(1);
        CHECK_THAT(f.fov, WithinAbs(0.6911, 1e-4));
        CHECK_THAT(f.aspect_ratio, WithinAbs(1.7778, 1e-4));
        CHECK_THAT(f.near_clip, WithinAbs(0.1, 1e-6));
        CHECK_THAT(f.far_clip, WithinAbs(100.0, 1e-6));
    }

    SECTION("basis vectors extracted from matrix_world") {
        const auto& f = loader.getFrame(1);
        // Position from column 3: (5, 3, -10)
        CHECK_THAT(f.position[0], WithinAbs(5.0, 1e-4));
        CHECK_THAT(f.position[1], WithinAbs(3.0, 1e-4));
        CHECK_THAT(f.position[2], WithinAbs(-10.0, 1e-4));
        // Right from column 0: (1, 0, 0)
        CHECK_THAT(f.right[0], WithinAbs(1.0, 1e-4));
        CHECK_THAT(f.right[1], WithinAbs(0.0, 1e-4));
        CHECK_THAT(f.right[2], WithinAbs(0.0, 1e-4));
        // Up from column 1: (0, 1, 0)
        CHECK_THAT(f.up[0], WithinAbs(0.0, 1e-4));
        CHECK_THAT(f.up[1], WithinAbs(1.0, 1e-4));
        CHECK_THAT(f.up[2], WithinAbs(0.0, 1e-4));
        // Forward from column 2: (0, 0, 1)
        CHECK_THAT(f.forward[0], WithinAbs(0.0, 1e-4));
        CHECK_THAT(f.forward[1], WithinAbs(0.0, 1e-4));
        CHECK_THAT(f.forward[2], WithinAbs(1.0, 1e-4));
    }

    SECTION("getFrame throws for missing frame") {
        CHECK_THROWS_AS(loader.getFrame(99), std::out_of_range);
    }

    // cleanup
    std::remove(path.c_str());
}

// =========================================================================
// Scenario 2: Invalid/malformed JSON rejection
// =========================================================================
TEST_CASE("CameraDataLoader rejects malformed JSON", "[camera]") {
    CameraDataLoader loader;
    std::string err;

    SECTION("non-existent file") {
        CHECK_FALSE(loader.load("nonexistent_file.json", err));
        CHECK(err.find("Failed to open") != std::string::npos);
    }

    SECTION("invalid JSON syntax") {
        auto path = writeTempJson("{not valid json", "test_camera_bad_syntax.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("parse error") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("missing version field") {
        auto path = writeTempJson(R"({"render_width":1920,"render_height":1080,"frames":{}})",
                                  "test_camera_no_version.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("version") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("missing frames object") {
        auto path = writeTempJson(R"({"version":1,"render_width":1920,"render_height":1080})",
                                  "test_camera_no_frames.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("frames") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("frame missing matrix_world") {
        auto path = writeTempJson(R"({
            "version":1,"render_width":1920,"render_height":1080,
            "frames":{"0001":{"projection":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
            "fov":0.5,"aspect_ratio":1.78,"near_clip":0.1,"far_clip":100}}
        })", "test_camera_no_mw.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("matrix_world") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("matrix_world wrong dimensions") {
        auto path = writeTempJson(R"({
            "version":1,"render_width":1920,"render_height":1080,
            "frames":{"0001":{"matrix_world":[[1,0,0],[0,1,0],[0,0,1]],
            "projection":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
            "fov":0.5,"aspect_ratio":1.78,"near_clip":0.1,"far_clip":100}}
        })", "test_camera_bad_mw.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("4x4") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("empty frames") {
        auto path = writeTempJson(R"({"version":1,"render_width":1920,"render_height":1080,"frames":{}})",
                                  "test_camera_empty_frames.json");
        CHECK_FALSE(loader.load(path, err));
        CHECK(err.find("No frames") != std::string::npos);
        std::remove(path.c_str());
    }
}

// =========================================================================
// Scenario 3: Derived camera parameters (computePairParams)
// =========================================================================
TEST_CASE("CameraDataLoader computePairParams produces valid derived data", "[camera]") {
    auto path = writeTempJson(kValidCameraJson, "test_camera_derived.json");
    CameraDataLoader loader;
    std::string err;
    REQUIRE(loader.load(path, err));

    DlssFgCameraParams params{};

    SECTION("identity case: same frame as current and previous") {
        // When current == previous, clipToPrevClip should be identity
        // Use frame 1 for both
        auto identityJson = R"({
            "version": 1, "render_width": 1920, "render_height": 1080,
            "frames": {
                "0001": {
                    "matrix_world": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
                    "projection": [[2,0,0,0],[0,2,0,0],[0,0,-1.002,-0.2002],[0,0,-1,0]],
                    "fov": 0.6911, "aspect_ratio": 1.7778, "near_clip": 0.1, "far_clip": 100.0
                },
                "0002": {
                    "matrix_world": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
                    "projection": [[2,0,0,0],[0,2,0,0],[0,0,-1.002,-0.2002],[0,0,-1,0]],
                    "fov": 0.6911, "aspect_ratio": 1.7778, "near_clip": 0.1, "far_clip": 100.0
                }
            }
        })";
        auto pathId = writeTempJson(identityJson, "test_camera_identity.json");
        CameraDataLoader idLoader;
        std::string idErr;
        REQUIRE(idLoader.load(pathId, idErr));

        DlssFgCameraParams idParams{};
        REQUIRE(idLoader.computePairParams(2, 1, idParams, idErr));

        // clipToPrevClip should be identity when cameras are the same
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                CHECK_THAT(idParams.clipToPrevClip[r][c], WithinAbs(expected, 1e-4));
            }
        }

        // prevClipToClip should also be identity
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                CHECK_THAT(idParams.prevClipToClip[r][c], WithinAbs(expected, 1e-4));
            }
        }

        std::remove(pathId.c_str());
    }

    SECTION("non-trivial pair produces valid matrices") {
        REQUIRE(loader.computePairParams(2, 1, params, err));

        // cameraViewToClip should match frame 2's projection
        const auto& f2 = loader.getFrame(2);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK_THAT(params.cameraViewToClip[r][c],
                           WithinAbs(f2.projection[r][c], 1e-4));

        // clipToLensClip should be identity
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK_THAT(params.clipToLensClip[r][c],
                           WithinAbs((r == c) ? 1.0f : 0.0f, 1e-6));

        // Camera position from frame 2
        CHECK_THAT(params.cameraPos[0], WithinAbs(6.0, 1e-4));
        CHECK_THAT(params.cameraPos[1], WithinAbs(3.0, 1e-4));
        CHECK_THAT(params.cameraPos[2], WithinAbs(-9.0, 1e-4));

        CHECK_THAT(params.nearPlane, WithinAbs(0.1, 1e-6));
        CHECK_THAT(params.farPlane, WithinAbs(100.0, 1e-6));
        CHECK_THAT(params.fov, WithinAbs(0.6911, 1e-4));
        CHECK_THAT(params.aspectRatio, WithinAbs(1.7778, 1e-4));
    }

    SECTION("clipToCameraView is inverse of cameraViewToClip") {
        REQUIRE(loader.computePairParams(2, 1, params, err));

        // Multiply cameraViewToClip * clipToCameraView -> should be identity
        float product[4][4];
        MatrixUtil::multiply(product, params.cameraViewToClip, params.clipToCameraView);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK_THAT(product[r][c],
                           WithinAbs((r == c) ? 1.0f : 0.0f, 1e-3));
    }

    SECTION("prevClipToClip is inverse of clipToPrevClip") {
        REQUIRE(loader.computePairParams(2, 1, params, err));

        float product[4][4];
        MatrixUtil::multiply(product, params.clipToPrevClip, params.prevClipToClip);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK_THAT(product[r][c],
                           WithinAbs((r == c) ? 1.0f : 0.0f, 1e-3));
    }

    SECTION("missing frame returns error") {
        CHECK_FALSE(loader.computePairParams(99, 1, params, err));
        CHECK(err.find("not found") != std::string::npos);
    }

    std::remove(path.c_str());
}

// =========================================================================
// MatrixUtil tests
// =========================================================================
TEST_CASE("MatrixUtil identity", "[camera][matrix]") {
    float m[4][4];
    MatrixUtil::identity(m);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_THAT(m[r][c], WithinAbs((r == c) ? 1.0f : 0.0f, 1e-6));
}

TEST_CASE("MatrixUtil multiply with identity", "[camera][matrix]") {
    float a[4][4] = {
        {1, 2, 3, 4},
        {5, 6, 7, 8},
        {9, 10, 11, 12},
        {13, 14, 15, 16}
    };
    float id[4][4];
    MatrixUtil::identity(id);

    float result[4][4];
    MatrixUtil::multiply(result, a, id);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_THAT(result[r][c], WithinAbs(a[r][c], 1e-6));
}

TEST_CASE("MatrixUtil inverse of known matrix", "[camera][matrix]") {
    // Translation matrix: translate by (3, 4, 5)
    float m[4][4] = {
        {1, 0, 0, 3},
        {0, 1, 0, 4},
        {0, 0, 1, 5},
        {0, 0, 0, 1}
    };
    float inv[4][4];
    REQUIRE(MatrixUtil::inverse(inv, m));

    // Inverse should translate by (-3, -4, -5)
    CHECK_THAT(inv[0][3], WithinAbs(-3.0, 1e-5));
    CHECK_THAT(inv[1][3], WithinAbs(-4.0, 1e-5));
    CHECK_THAT(inv[2][3], WithinAbs(-5.0, 1e-5));

    // M * M^-1 = I
    float product[4][4];
    MatrixUtil::multiply(product, m, inv);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_THAT(product[r][c], WithinAbs((r == c) ? 1.0f : 0.0f, 1e-5));
}

TEST_CASE("MatrixUtil inverse of singular matrix returns false", "[camera][matrix]") {
    float m[4][4] = {
        {1, 2, 3, 4},
        {2, 4, 6, 8},  // row 1 = 2 * row 0
        {0, 0, 1, 0},
        {0, 0, 0, 1}
    };
    float inv[4][4];
    CHECK_FALSE(MatrixUtil::inverse(inv, m));
    // Should return identity on failure
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_THAT(inv[r][c], WithinAbs((r == c) ? 1.0f : 0.0f, 1e-6));
}
