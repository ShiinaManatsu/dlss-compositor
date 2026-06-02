# =============================================================================
# Catch2 test harness for dlss_engine_lib.
#
# Real test sources are added in later tasks. This stub keeps `ctest` runnable
# from day one without requiring any test files.
# =============================================================================

# Catch2 v3 is found at the top-level CMakeLists.txt scope.
add_executable(dlss_engine_tests test_smoke.cpp test_vulkan_context.cpp)
target_link_libraries(dlss_engine_tests PRIVATE
    dlss_engine_lib
    dlss_warnings
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(dlss_engine_tests)
