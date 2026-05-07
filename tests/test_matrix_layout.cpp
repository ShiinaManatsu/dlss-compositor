// If this test fails, MatrixUtil::multiply or MatrixUtil::inverse has a layout
// assumption mismatch -> triggers FG ghost artifacts / reversed motion bug

#include <catch2/catch_test_macros.hpp>

#include "core/camera_data_loader.h"

#include <cmath>
#include <string>

// ---------------------------------------------------------------------------
// Helper: derive fixture path from this source file's location
// ---------------------------------------------------------------------------
static std::string fixturePath() {
    std::string f = __FILE__;
    auto pos = f.find_last_of("/\\");
    return f.substr(0, pos) + "/fixtures/static_camera.json";
}

// ---------------------------------------------------------------------------
// Test 1: Static camera must produce identity reprojection matrices
// ---------------------------------------------------------------------------
TEST_CASE("Matrix layout: static camera produces identity reprojection",
          "[camera][matrix]") {
    CameraDataLoader loader;
    std::string err;

    REQUIRE(loader.load(fixturePath(), err));
    REQUIRE(err.empty());

    REQUIRE(loader.hasFrame(200));
    REQUIRE(loader.hasFrame(201));

    DlssFgCameraParams params{};
    REQUIRE(loader.computePairParams(201, 200, params, err));
    REQUIRE(err.empty());

    const float tol = 1e-5f;

    SECTION("clipToPrevClip is identity") {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                INFO("clipToPrevClip[" << r << "][" << c << "] = "
                     << params.clipToPrevClip[r][c] << " (expected "
                     << expected << ")");
                REQUIRE(std::abs(params.clipToPrevClip[r][c] - expected) < tol);
            }
        }
    }

    SECTION("prevClipToClip is identity") {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                INFO("prevClipToClip[" << r << "][" << c << "] = "
                     << params.prevClipToClip[r][c] << " (expected "
                     << expected << ")");
                REQUIRE(std::abs(params.prevClipToClip[r][c] - expected) < tol);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Test 2: MatrixUtil multiply/inverse consistency with identity
// ---------------------------------------------------------------------------
TEST_CASE("Matrix layout: identity world matrix preserves layout",
          "[camera][matrix]") {
    const float tol = 1e-6f;

    SECTION("multiply(identity, identity) == identity") {
        float id[4][4];
        MatrixUtil::identity(id);

        float result[4][4];
        MatrixUtil::multiply(result, id, id);

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                INFO("multiply result[" << r << "][" << c << "] = "
                     << result[r][c] << " (expected " << expected << ")");
                REQUIRE(std::abs(result[r][c] - expected) < tol);
            }
        }
    }

    SECTION("inverse(identity) == identity") {
        float id[4][4];
        MatrixUtil::identity(id);

        float inv[4][4];
        REQUIRE(MatrixUtil::inverse(inv, id));

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                INFO("inverse result[" << r << "][" << c << "] = "
                     << inv[r][c] << " (expected " << expected << ")");
                REQUIRE(std::abs(inv[r][c] - expected) < tol);
            }
        }
    }
}
