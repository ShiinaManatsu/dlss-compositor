# Catch2 test runner
add_executable(dlss-compositor-tests
    test_scaffold.cpp
)
target_link_libraries(dlss-compositor-tests PRIVATE
    Catch2::Catch2WithMain
)
target_include_directories(dlss-compositor-tests PRIVATE
    ${tinyexr_SOURCE_DIR}
)
include(CTest)
include(Catch)
catch_discover_tests(dlss-compositor-tests)
