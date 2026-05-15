# Catch2 test runner
add_executable(dlss-compositor-tests
    test_scaffold.cpp
    test_exr_reader.cpp
    test_exr_channel_aliases.cpp
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
    test_sharpen_processor.cpp
    test_integration.cpp
    test_matrix_layout.cpp
    test_ndjson_emitter.cpp
    test_preset_mapping.cpp
    test_cli_advanced_flags.cpp
    test_camera_cut_detection.cpp
    test_sequence_filename_filter.cpp
    test_ngx_banner.cpp
    test_integration_preset_trace.cpp
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
add_test(NAME test_dlss_sr_processor COMMAND dlss-compositor-tests "dlss_sr_single_evaluation")
set_tests_properties(test_dlss_sr_processor PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME test_sequence_processor COMMAND dlss-compositor-tests "[sequence]")
set_tests_properties(test_sequence_processor PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME integration COMMAND dlss-compositor-tests "[integration]")
set_tests_properties(integration PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME test_matrix_layout COMMAND dlss-compositor-tests "[matrix]")
set_tests_properties(test_matrix_layout PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME CliAdvancedFlags COMMAND dlss-compositor-tests "[CliAdvancedFlags]")
set_tests_properties(CliAdvancedFlags PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME CameraCut COMMAND dlss-compositor-tests "[CameraCut]")
set_tests_properties(CameraCut PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME SequenceFilenameFilter COMMAND dlss-compositor-tests "[SequenceFilenameFilter]")
set_tests_properties(SequenceFilenameFilter PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME NgxBanner COMMAND dlss-compositor-tests "[NgxBanner]")
set_tests_properties(NgxBanner PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME ExrChannelAliases COMMAND dlss-compositor-tests "[ExrChannelAliases]")
set_tests_properties(ExrChannelAliases PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
add_test(NAME IntegrationPresetTrace COMMAND dlss-compositor-tests "[IntegrationPresetTrace]")
set_tests_properties(IntegrationPresetTrace PROPERTIES
    SKIP_REGULAR_EXPRESSION "SKIPPED:"
    LABELS "gpu_required"
)

if(MSVC)
    add_custom_command(TARGET dlss-compositor-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            "$<TARGET_FILE_DIR:dlss-compositor-tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Catch2::Catch2>"
            "$<TARGET_FILE_DIR:dlss-compositor-tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Catch2::Catch2WithMain>"
            "$<TARGET_FILE_DIR:dlss-compositor-tests>"
    )
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
