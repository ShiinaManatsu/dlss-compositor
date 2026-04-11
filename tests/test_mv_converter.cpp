#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "../src/core/mv_converter.h"

TEST_CASE("MvConverter - 10px right motion converts to (-10, 0)", "[mv]") {
    const int W = 2;
    const int H = 2;
    std::vector<float> mv4(W * H * 4, 0.0f);

    for (int i = 0; i < W * H; ++i) {
        mv4[i * 4 + 0] = 10.0f;
        mv4[i * 4 + 1] = 0.0f;
    }

    auto result = MvConverter::convert(mv4.data(), W, H);

    REQUIRE(result.mvXY.size() == static_cast<size_t>(W * H * 2));
    REQUIRE(std::fabs(result.mvXY[0] - 10.0f) < 1e-5f);
    REQUIRE(std::fabs(result.mvXY[1] - 0.0f) < 1e-5f);
}

TEST_CASE("MvConverter - Y axis", "[mv]") {
    const int W = 1;
    const int H = 1;
    std::vector<float> mv4 = {0.0f, 5.0f, 0.0f, 0.0f};

    auto result = MvConverter::convert(mv4.data(), W, H);

    REQUIRE(std::fabs(result.mvXY[1] - 5.0f) < 1e-5f);
}

TEST_CASE("MvConverter - zero motion vectors", "[mv]") {
    const int W = 4;
    const int H = 4;
    std::vector<float> mv4(W * H * 4, 0.0f);

    auto result = MvConverter::convert(mv4.data(), W, H);

    for (size_t i = 0; i < result.mvXY.size(); ++i) {
        REQUIRE(std::fabs(result.mvXY[i] - 0.0f) < 1e-5f);
    }
}

TEST_CASE("MvConverter - scale factors for 1920x1080", "[mv]") {
    const int W = 1920;
    const int H = 1080;
    std::vector<float> mv4(W * H * 4, 0.0f);

    auto result = MvConverter::convert(mv4.data(), W, H);

    REQUIRE(std::fabs(result.scaleX - 1.0f) < 1e-7f);
    REQUIRE(std::fabs(result.scaleY - 1.0f) < 1e-7f);
}

TEST_CASE("MvConverter - scale factors for 3840x2160", "[mv]") {
    const int W = 3840;
    const int H = 2160;
    std::vector<float> mv4(W * H * 4, 0.0f);

    auto result = MvConverter::convert(mv4.data(), W, H);

    REQUIRE(std::fabs(result.scaleX - 1.0f) < 1e-7f);
    REQUIRE(std::fabs(result.scaleY - 1.0f) < 1e-7f);
}

TEST_CASE("MvConverter - only channels 0,1 used (channels 2,3 ignored)", "[mv]") {
    const int W = 1;
    const int H = 1;
    std::vector<float> mv4 = {3.0f, 4.0f, 999.0f, 888.0f};

    auto result = MvConverter::convert(mv4.data(), W, H);

    REQUIRE(std::fabs(result.mvXY[0] - 3.0f) < 1e-5f);
    REQUIRE(std::fabs(result.mvXY[1] - 4.0f) < 1e-5f);
}
