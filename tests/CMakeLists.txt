# =============================================================================
# Catch2 test harness for dlss_engine_lib.
#
# Real test sources are added in later tasks. This stub keeps `ctest` runnable
# from day one without requiring any test files.
# =============================================================================

# Catch2 v3 is found at the top-level CMakeLists.txt scope.
add_executable(dlss_engine_tests
    test_smoke.cpp
    test_vulkan_context.cpp
    test_ngx_init.cpp
    test_frame_prefetcher.cpp
    test_channel_mapper.cpp
    test_cli_config_executor.cpp
    test_texture_pool.cpp
    test_depth_pass.cpp
    test_dlss_fg.cpp
    test_dlss_sr.cpp
    test_tonemap_pass.cpp
    test_inverse_tonemap.cpp
    test_mv_conversion_sanity.cpp
    test_async_writer.cpp
    test_rpc_dispatcher.cpp
    test_fg_only_pipeline.cpp
    test_sr_fg_integration.cpp
    test_sr_only_pipeline.cpp)
target_link_libraries(dlss_engine_tests PRIVATE
    dlss_engine_lib
    dlss_warnings
    volk::volk
    Catch2::Catch2WithMain)
target_compile_definitions(dlss_engine_tests PRIVATE VK_NO_PROTOTYPES)
if(WIN32)
    target_link_libraries(dlss_engine_tests PRIVATE version.lib)
endif()

# GPU NGX tests need the same runtime DLL side-by-side layout as the CLI.
foreach(_dll_name nvngx_dlss.dll nvngx_dlssg.dll)
    set(_dev_dll "${DLSS_DLL_DEV_DIR}/${_dll_name}")
    set(_rel_dll "${DLSS_DLL_REL_DIR}/${_dll_name}")
    if(EXISTS "${_dev_dll}" AND EXISTS "${_rel_dll}")
        add_custom_command(TARGET dlss_engine_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<$<CONFIG:Debug>:${_dev_dll}>$<$<NOT:$<CONFIG:Debug>>:${_rel_dll}>"
                "$<TARGET_FILE_DIR:dlss_engine_tests>"
            COMMENT "Copying DLSS runtime DLL for tests (${_dll_name}; Debug=dev, non-Debug=rel)"
            VERBATIM)
    endif()
endforeach()

include(Catch)
catch_discover_tests(dlss_engine_tests)

add_executable(dlss_cli_tests
    test_cli_config_executor.cpp)
target_link_libraries(dlss_cli_tests PRIVATE
    dlss_engine_lib
    dlss_warnings
    volk::volk
    Catch2::Catch2WithMain)
if(WIN32)
    target_link_libraries(dlss_cli_tests PRIVATE version.lib)
endif()
catch_discover_tests(dlss_cli_tests)
