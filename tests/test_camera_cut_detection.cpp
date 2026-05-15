#include <catch2/catch_test_macros.hpp>

#include "core/camera_data_loader.h"

namespace {

CameraFrameData makeFrame(float px, float py, float pz,
                          float fx, float fy, float fz) {
    CameraFrameData f{};
    f.position[0] = px;
    f.position[1] = py;
    f.position[2] = pz;
    f.forward[0] = fx;
    f.forward[1] = fy;
    f.forward[2] = fz;
    return f;
}

} // namespace

TEST_CASE("CameraCut large translation triggers cut", "[CameraCut][unit]") {
    const CameraFrameData prev = makeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const CameraFrameData curr = makeFrame(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    REQUIRE(isCameraCut(prev, curr, 5.0f, 30.0f));
}

TEST_CASE("CameraCut large rotation triggers cut", "[CameraCut][unit]") {
    const CameraFrameData prev = makeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const CameraFrameData curr = makeFrame(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

    REQUIRE(isCameraCut(prev, curr, 5.0f, 30.0f));
}

TEST_CASE("CameraCut sub-threshold does not trigger", "[CameraCut][unit]") {
    const CameraFrameData prev = makeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const CameraFrameData curr = makeFrame(4.9f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    REQUIRE_FALSE(isCameraCut(prev, curr, 5.0f, 30.0f));
}

TEST_CASE("CameraCut detection respects custom threshold", "[CameraCut][unit]") {
    const CameraFrameData prev = makeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const CameraFrameData curr = makeFrame(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    REQUIRE_FALSE(isCameraCut(prev, curr, 20.0f, 30.0f));
    REQUIRE(isCameraCut(prev, curr, 5.0f, 30.0f));
}

TEST_CASE("CameraCut first-frame scenario", "[CameraCut][unit]") {
    const CameraFrameData frame = makeFrame(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    REQUIRE_FALSE(isCameraCut(frame, frame, 5.0f, 30.0f));
}
