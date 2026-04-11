# Catch2 test runner
add_executable(dlss-compositor-tests
    test_scaffold.cpp
    test_exr_reader.cpp
    test_channel_mapper.cpp
    test_mv_converter.cpp
    test_frame_prefetcher.cpp
    test_texture_pool.cpp
    test_cli.cpp
    test_texture_pipeline.cpp
    test_dlss_rr_processor.cpp
    test_camera_data_loader.cpp
    test_dlss_fg_processor.cpp
    test_sequence_processor.cpp
    test_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/cli_parser.cpp
)
target_link_libraries(dlss-compositor-tests PRIVATE
    Catch2::Catch2WithMain
    dlss_compositor_core
)
target_include_directories(dlss-compositor-tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/cli
    ${CMAKE_SOURCE_DIR}/src/core
)
include(CTest)
include(Catch)
catch_discover_tests(dlss-compositor-tests)
add_test(NAME test_dlss_rr_processor COMMAND dlss-compositor-tests "dlss_rr_single_evaluation")
set_tests_properties(test_dlss_rr_processor PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME test_sequence_processor COMMAND dlss-compositor-tests "[sequence]")
set_tests_properties(test_sequence_processor PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME integration COMMAND dlss-compositor-tests "[integration]")
set_tests_properties(integration PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")

if(MSVC)
    file(GLOB DLSS_NGXCORE_DLLS "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
    foreach(DLL ${DLSS_NGXCORE_DLLS})
        add_custom_command(TARGET dlss-compositor-tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:dlss-compositor-tests>
        )
    endforeach()
    file(GLOB DLSS_NGXCORE_FG_DLLS "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")
    foreach(DLL ${DLSS_NGXCORE_FG_DLLS})
        add_custom_command(TARGET dlss-compositor-tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:dlss-compositor-tests>
        )
    endforeach()
endif()
