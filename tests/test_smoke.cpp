// =============================================================================
// Smoke test for dlss_engine_lib — keeps the test binary buildable before any
// real tests exist. Real coverage is added in later tasks.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

TEST_CASE("engine library smoke", "[smoke]") {
    REQUIRE(1 + 1 == 2);
}
