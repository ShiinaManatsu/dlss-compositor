#include <catch2/catch_test_macros.hpp>

#include "core/types.h"

TEST_CASE("DLSS constant tables match spec", "[core][constants]") {
    REQUIRE(dlss::DLSS_RATIO[(int)dlss::QualityMode::DLAA] == 1.0f);
    REQUIRE(dlss::DLSS_RATIO[(int)dlss::QualityMode::Performance] == 2.0f);
    REQUIRE(dlss::DLSS_RATIO[(int)dlss::QualityMode::UltraPerformance] == 3.0f);
    REQUIRE(dlss::JITTER_PHASES[(int)dlss::QualityMode::UltraPerformance] == 72);
    REQUIRE(dlss::JITTER_PHASES[(int)dlss::QualityMode::Balanced] == 24);
    REQUIRE(dlss::DEFAULT_PRESET[(int)dlss::QualityMode::Performance] == dlss::NgxPreset::M);
    REQUIRE(dlss::DEFAULT_PRESET[(int)dlss::QualityMode::UltraPerformance] == dlss::NgxPreset::L);
}
