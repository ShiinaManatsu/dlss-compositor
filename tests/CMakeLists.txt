# Catch2 test runner
add_executable(dlss-compositor-tests
    test_scaffold.cpp
    test_exr_reader.cpp
    test_channel_mapper.cpp
    test_mv_converter.cpp
)
target_link_libraries(dlss-compositor-tests PRIVATE
    Catch2::Catch2WithMain
    dlss_compositor_core
)
target_include_directories(dlss-compositor-tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${tinyexr_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}/deps/miniz
)
include(CTest)
include(Catch)
catch_discover_tests(dlss-compositor-tests)
