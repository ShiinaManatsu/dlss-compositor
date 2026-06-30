# =============================================================================
# DLSS Compositor 1.0.0 — Headless compute pipeline (engine lib + CLI exe)
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# vcpkg toolchain must be loaded before project() if not already set via preset.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(DlssCompositor
    VERSION     1.0.0
    DESCRIPTION "Headless DLSS Super-Resolution + Frame-Generation compute compositor"
    LANGUAGES   CXX)

set(DLSS_COMPOSITOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# Global C++ settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Place all binaries in build/<config>/ for easy DLL-side-by-side runtime layout.
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${_cfg} "${CMAKE_BINARY_DIR}/$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${_cfg} "${CMAKE_BINARY_DIR}/$<CONFIG>")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${_cfg} "${CMAKE_BINARY_DIR}/$<CONFIG>")
endforeach()

# -----------------------------------------------------------------------------
# DLSS SDK location (mandatory)
#
#   - Unset   -> auto-default to ./DLSS submodule if its headers exist.
#   - Empty   -> hard FATAL_ERROR (explicit user error).
#   - Set     -> validated below.
# -----------------------------------------------------------------------------
if(DEFINED DLSS_SDK_ROOT AND DLSS_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR
        "DLSS_SDK_ROOT must be set to the DLSS submodule path "
        "(received empty string). Pass -DDLSS_SDK_ROOT=<path> or unset it "
        "to auto-default to ./DLSS.")
endif()

if(NOT DEFINED DLSS_SDK_ROOT)
    if(EXISTS "${DLSS_COMPOSITOR_SOURCE_DIR}/DLSS/include/nvsdk_ngx_vk.h")
        set(DLSS_SDK_ROOT "${DLSS_COMPOSITOR_SOURCE_DIR}/DLSS" CACHE PATH "DLSS SDK root")
        message(STATUS "DLSS_SDK_ROOT not set; defaulting to submodule at ${DLSS_SDK_ROOT}")
    else()
        message(FATAL_ERROR
            "DLSS_SDK_ROOT must be set to the DLSS submodule path. "
            "Run `git submodule update --init DLSS` then re-configure with "
            "-DDLSS_SDK_ROOT=<path-to-DLSS>.")
    endif()
endif()

if(NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR
        "DLSS_SDK_ROOT='${DLSS_SDK_ROOT}' does not contain include/nvsdk_ngx_vk.h. "
        "Did you initialize the DLSS submodule?")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64")
set(DLSS_DLL_DEV_DIR "${DLSS_SDK_ROOT}/lib/Windows_x86_64/dev")
set(DLSS_DLL_REL_DIR "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel")
set(DLSS_LIB_RELEASE "${DLSS_LIB_DIR}/nvsdk_ngx_d.lib")
set(DLSS_LIB_DEBUG   "${DLSS_LIB_DIR}/nvsdk_ngx_d_dbg.lib")
foreach(_lib IN LISTS DLSS_LIB_RELEASE DLSS_LIB_DEBUG)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "DLSS import lib not found at: ${_lib}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Compiler flags (MSVC strict)
# -----------------------------------------------------------------------------
add_library(dlss_warnings INTERFACE)
if(MSVC)
    target_compile_options(dlss_warnings INTERFACE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /MP
        /EHsc
        /D_CRT_SECURE_NO_WARNINGS
        /DNOMINMAX
        /DWIN32_LEAN_AND_MEAN)
else()
    target_compile_options(dlss_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# -----------------------------------------------------------------------------
# Third-party dependencies (via vcpkg manifest)
# -----------------------------------------------------------------------------
macro(dlss_find_package_external)
    set(_DLSS_PREV_SUPPRESS_DEV_WARNINGS "${CMAKE_SUPPRESS_DEVELOPER_WARNINGS}")
    set(_DLSS_PREV_WARN_DEPRECATED "${CMAKE_WARN_DEPRECATED}")
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1)
    set(CMAKE_WARN_DEPRECATED OFF)
    find_package(${ARGV})
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS "${_DLSS_PREV_SUPPRESS_DEV_WARNINGS}")
    set(CMAKE_WARN_DEPRECATED "${_DLSS_PREV_WARN_DEPRECATED}")
    unset(_DLSS_PREV_SUPPRESS_DEV_WARNINGS)
    unset(_DLSS_PREV_WARN_DEPRECATED)
endmacro()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog        CONFIG REQUIRED)
find_package(fmt           CONFIG REQUIRED)
find_package(Catch2 3      CONFIG REQUIRED)
find_package(volk          CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Imath         CONFIG REQUIRED)
find_package(OpenEXR       CONFIG REQUIRED)
# OpenImageIO currently pulls OpenColorIO/expat configs that emit CMake
# developer/deprecation noise from third-party package files on Windows.
# Suppress those transitive warnings locally so our own configure warnings stay visible.
dlss_find_package_external(OpenImageIO CONFIG REQUIRED)
find_package(ZLIB          REQUIRED)
find_package(FFMPEG        REQUIRED)

get_target_property(_DLSS_SPDLOG_INCLUDE_DIRS spdlog::spdlog_header_only INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_DLSS_FMT_INCLUDE_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
add_library(dlss_log_headers INTERFACE)
target_include_directories(dlss_log_headers INTERFACE
    ${_DLSS_SPDLOG_INCLUDE_DIRS}
    ${_DLSS_FMT_INCLUDE_DIRS})
target_compile_definitions(dlss_log_headers INTERFACE
    SPDLOG_HEADER_ONLY
    SPDLOG_FMT_EXTERNAL
    FMT_HEADER_ONLY)

# Vulkan headers come transitively from volk, so find_package(Vulkan) is optional.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Found Vulkan SDK: ${Vulkan_INCLUDE_DIRS}")
else()
    message(STATUS "Vulkan SDK not found via find_package; relying on volk-bundled headers.")
endif()

# Lightweight CLI support path used by dlss-compositor-cli.  This intentionally
# avoids Vulkan/NGX objects so --config / JSON-RPC smoke flows can build and run
# even when GPU toolchain pieces are unavailable or under repair.
add_library(dlss_cli_support STATIC
    dlss-plugin/src/cli/config_executor.cpp
    dlss-plugin/src/cli/preview_ipc.cpp
    dlss-plugin/src/core/dlssconfig.cpp
    dlss-plugin/src/core/lut_loader.cpp
    dlss-plugin/src/core/log.cpp
    dlss-plugin/src/io/async_writer.cpp
    dlss-plugin/src/io/prores_writer.cpp)

target_include_directories(dlss_cli_support PUBLIC ${DLSS_COMPOSITOR_SOURCE_DIR}/dlss-plugin/src ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(dlss_cli_support
    PUBLIC
        nlohmann_json::nlohmann_json
        dlss_log_headers
        GPUOpen::VulkanMemoryAllocator
        Imath::Imath
        OpenEXR::OpenEXR
        OpenImageIO::OpenImageIO
        ZLIB::ZLIB
        ${FFMPEG_LIBRARIES}
    PRIVATE
        dlss_warnings)

# -----------------------------------------------------------------------------
# Engine static library — populated incrementally by later tasks (T4..T30).
# -----------------------------------------------------------------------------
add_library(dlss_engine_lib STATIC
    dlss-plugin/src/cli/config_executor.cpp
    dlss-plugin/src/cli/preview_ipc.cpp
    dlss-plugin/src/core/dlssconfig.cpp
    dlss-plugin/src/core/lut_loader.cpp
    dlss-plugin/src/core/log.cpp
    dlss-plugin/src/core/stub.cpp
    dlss-plugin/src/dlss/dlss_fg.cpp
    dlss-plugin/src/dlss/dlss_sr.cpp
    dlss-plugin/src/dlss/ngx_wrapper.cpp
    dlss-plugin/src/gpu/vulkan_context.cpp
    dlss-plugin/src/gpu/texture_pool.cpp
    dlss-plugin/src/gpu/passes/depth_pass.cpp
    dlss-plugin/src/gpu/passes/lut_inverse_pass.cpp
    dlss-plugin/src/gpu/passes/lut_tonemap_pass.cpp
    dlss-plugin/src/gpu/passes/mv_pass.cpp
    dlss-plugin/src/gpu/passes/prores_convert_pass.cpp
    dlss-plugin/src/gpu/passes/preview_pass.cpp
    dlss-plugin/src/gpu/passes/tonemap_pass.cpp
    dlss-plugin/src/gpu/passes/inverse_tonemap_pass.cpp
    dlss-plugin/src/io/frame_prefetcher.cpp
    dlss-plugin/src/io/channel_mapper.cpp
    dlss-plugin/src/io/async_writer.cpp
    dlss-plugin/src/io/prores_writer.cpp
    dlss-plugin/src/pipeline/fg_camera.cpp
    dlss-plugin/src/pipeline/fg_only_pipeline.cpp
    dlss-plugin/src/pipeline/sr_fg_pipeline.cpp
    dlss-plugin/src/pipeline/sr_only_pipeline.cpp
    dlss-plugin/src/rpc/dispatcher.cpp
    dlss-plugin/src/rpc/transport_stdio.cpp
    dlss-plugin/src/rpc/transport_native.cpp)

target_include_directories(dlss_engine_lib
    PUBLIC
        ${DLSS_COMPOSITOR_SOURCE_DIR}/dlss-plugin/src
        ${FFMPEG_INCLUDE_DIRS})

target_include_directories(dlss_engine_lib SYSTEM PUBLIC
        ${DLSS_SDK_ROOT}/include)

target_link_libraries(dlss_engine_lib
    PUBLIC
        nlohmann_json::nlohmann_json
        dlss_log_headers
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        Imath::Imath
        OpenEXR::OpenEXR
        OpenImageIO::OpenImageIO
        ZLIB::ZLIB
        ${FFMPEG_LIBRARIES}
    PRIVATE
        dlss_warnings
        $<$<CONFIG:Debug>:${DLSS_LIB_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${DLSS_LIB_RELEASE}>)

if(Vulkan_FOUND)
    target_link_libraries(dlss_engine_lib PUBLIC Vulkan::Headers)
endif()

if(WIN32)
    target_link_libraries(dlss_engine_lib PUBLIC version.lib)
endif()

target_compile_definitions(dlss_engine_lib PRIVATE
    VK_NO_PROTOTYPES
    DLSS_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders")

# -----------------------------------------------------------------------------
# CLI executable — full CLI front-end (T31): JSON-RPC stdio + --self-check + --config.
# -----------------------------------------------------------------------------
add_executable(dlss-compositor-cli dlss-plugin/src/main_cli.cpp)
target_link_libraries(dlss-compositor-cli PRIVATE dlss_engine_lib dlss_warnings)
target_link_libraries(dlss-compositor-cli PRIVATE volk::volk)
if(WIN32)
    target_link_libraries(dlss-compositor-cli PRIVATE version.lib)
endif()

# Copy DLSS runtime DLLs next to the exe so it can launch out-of-box.
# Debug uses the SDK dev DLLs for DLSS troubleshooting overlays; release builds
# always package the production DLLs from rel/.
foreach(_dll_name IN ITEMS nvngx_dlss.dll nvngx_dlssg.dll)
    set(_dev_dll "${DLSS_DLL_DEV_DIR}/${_dll_name}")
    set(_rel_dll "${DLSS_DLL_REL_DIR}/${_dll_name}")
    if(EXISTS "${_dev_dll}" AND EXISTS "${_rel_dll}")
        add_custom_command(TARGET dlss-compositor-cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<$<CONFIG:Debug>:${_dev_dll}>$<$<NOT:$<CONFIG:Debug>>:${_rel_dll}>"
                "$<TARGET_FILE_DIR:dlss-compositor-cli>"
            COMMENT "Copying DLSS runtime DLL for CLI (${_dll_name}; Debug=dev, non-Debug=rel)"
            VERBATIM)
    else()
        message(WARNING "DLSS runtime DLL missing (skipped copy): ${_dll_name}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# GLSL → SPIR-V helper.
# -----------------------------------------------------------------------------
find_program(GLSLC_EXECUTABLE glslc
    HINTS
        ENV VULKAN_SDK
        ENV VK_SDK_PATH
    PATH_SUFFIXES Bin bin)
find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator
    HINTS
        ENV VULKAN_SDK
        ENV VK_SDK_PATH
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/glslang"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows-static-md/tools/glslang"
    PATH_SUFFIXES Bin bin)
if(GLSLC_EXECUTABLE)
    set(DLSS_SHADER_COMPILER "${GLSLC_EXECUTABLE}")
    set(DLSS_SHADER_COMPILER_ARGS --target-env=vulkan1.3 -O)
    message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")
elseif(GLSLANG_VALIDATOR_EXECUTABLE)
    set(DLSS_SHADER_COMPILER "${GLSLANG_VALIDATOR_EXECUTABLE}")
    set(DLSS_SHADER_COMPILER_ARGS -V --target-env vulkan1.3)
    message(STATUS "Found glslangValidator: ${GLSLANG_VALIDATOR_EXECUTABLE}")
else()
    message(FATAL_ERROR "No GLSL shader compiler found. Install Vulkan SDK glslc or vcpkg glslang[tools].")
endif()

# Compile a single .comp shader to .spv and attach it to a target.
function(dlss_add_shader TARGET SHADER_SRC)
    get_filename_component(_name ${SHADER_SRC} NAME)
    set(_spv "${CMAKE_BINARY_DIR}/shaders/${_name}.spv")
    add_custom_command(
        OUTPUT  "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
        COMMAND ${DLSS_SHADER_COMPILER} ${DLSS_SHADER_COMPILER_ARGS}
                "${DLSS_COMPOSITOR_SOURCE_DIR}/${SHADER_SRC}" -o "${_spv}"
        DEPENDS "${DLSS_COMPOSITOR_SOURCE_DIR}/${SHADER_SRC}"
        COMMENT "GLSL ${SHADER_SRC} -> ${_spv}"
        VERBATIM)
    target_sources(${TARGET} PRIVATE "${_spv}")
    set_property(GLOBAL APPEND PROPERTY DLSS_SHADER_SPV_FILES "${_spv}")
endfunction()

# -----------------------------------------------------------------------------
# Shader compilation: depth world-linear → NDC (T22) + MV Blender→DLSS (T23)
# -----------------------------------------------------------------------------
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/depth_world_to_ndc.comp)
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/mv_blender_to_dlss.comp)
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/preview_display.comp)
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/prores_yuv422.comp)

# -----------------------------------------------------------------------------
# Shader compilation — Stage 4 tonemap + EOTF
# -----------------------------------------------------------------------------
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/tonemap_eotf.comp)
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/lut_tonemap.comp)
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/lut_inverse.comp)

# -----------------------------------------------------------------------------
# Shader compilation — Stage 6 inverse tonemap + EOTF
# -----------------------------------------------------------------------------
dlss_add_shader(dlss_engine_lib dlss-plugin/src/shaders/inverse_tonemap_eotf.comp)

get_property(_dlss_shader_spv_files GLOBAL PROPERTY DLSS_SHADER_SPV_FILES)
if(TARGET dlss-compositor-cli)
    add_custom_command(
        TARGET dlss-compositor-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dlss-compositor-cli>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlss_shader_spv_files} "$<TARGET_FILE_DIR:dlss-compositor-cli>"
        COMMENT "Copying compute shader SPIR-V next to CLI executable"
        VERBATIM)
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()
if(PROJECT_IS_TOP_LEVEL AND EXISTS "${DLSS_COMPOSITOR_SOURCE_DIR}/dlss-plugin/tests/CMakeLists.txt")
    add_subdirectory(dlss-plugin/tests)
endif()

# -----------------------------------------------------------------------------
# Status summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "DLSS Compositor v${PROJECT_VERSION}")
message(STATUS "  Generator       : ${CMAKE_GENERATOR}")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Toolchain file  : ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  vcpkg triplet   : ${VCPKG_TARGET_TRIPLET}")
message(STATUS "  DLSS SDK root   : ${DLSS_SDK_ROOT}")
message(STATUS "  DLSS lib (rel)  : ${DLSS_LIB_RELEASE}")
message(STATUS "  DLSS lib (dbg)  : ${DLSS_LIB_DEBUG}")
message(STATUS "")
