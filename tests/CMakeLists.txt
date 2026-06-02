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
    test_texture_pool.cpp
    test_depth_pass.cpp
    test_tonemap_pass.cpp
    test_mv_conversion_sanity.cpp
    test_async_writer.cpp)
target_link_libraries(dlss_engine_tests PRIVATE
    dlss_engine_lib
    dlss_warnings
    Catch2::Catch2WithMain)

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
