# =============================================================================
# DLSS Compositor v2 — Headless compute pipeline (engine lib + CLI exe)
# =============================================================================
cmake_minimum_required(VERSION 3.20)

# vcpkg toolchain must be loaded before project() if not already set via preset.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(DlssCompositor
    VERSION     2.0.0
    DESCRIPTION "Headless DLSS Super-Resolution + Frame-Generation compute compositor"
    LANGUAGES   CXX)

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
    if(EXISTS "${CMAKE_SOURCE_DIR}/DLSS/include/nvsdk_ngx_vk.h")
        set(DLSS_SDK_ROOT "${CMAKE_SOURCE_DIR}/DLSS" CACHE PATH "DLSS SDK root")
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
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog        CONFIG REQUIRED)
find_package(Catch2 3      CONFIG REQUIRED)
find_package(volk          CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Imath         CONFIG REQUIRED)
find_package(OpenEXR       CONFIG REQUIRED)

# Vulkan headers come transitively from volk, so find_package(Vulkan) is optional.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Found Vulkan SDK: ${Vulkan_INCLUDE_DIRS}")
else()
    message(STATUS "Vulkan SDK not found via find_package; relying on volk-bundled headers.")
endif()

# -----------------------------------------------------------------------------
# Engine static library — populated incrementally by later tasks (T4..T30).
# -----------------------------------------------------------------------------
add_library(dlss_engine_lib STATIC
    src/core/stub.cpp
    src/dlss/ngx_wrapper.cpp
    src/gpu/vulkan_context.cpp
    src/io/frame_prefetcher.cpp
    src/io/channel_mapper.cpp)

target_include_directories(dlss_engine_lib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    SYSTEM PUBLIC
        ${DLSS_SDK_ROOT}/include)

target_link_libraries(dlss_engine_lib
    PUBLIC
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        volk::volk_headers
        GPUOpen::VulkanMemoryAllocator
        Imath::Imath
        OpenEXR::OpenEXR
    PRIVATE
        dlss_warnings
        $<$<CONFIG:Debug>:${DLSS_LIB_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${DLSS_LIB_RELEASE}>)

if(Vulkan_FOUND)
    target_link_libraries(dlss_engine_lib PUBLIC Vulkan::Headers)
endif()

# -----------------------------------------------------------------------------
# CLI executable (smoke entry — replaced by full CLI in T31).
# -----------------------------------------------------------------------------
add_executable(dlss-compositor-cli src/main_smoke.cpp)
target_link_libraries(dlss-compositor-cli PRIVATE dlss_engine_lib dlss_warnings)

# Copy DLSS runtime DLLs next to the exe so it can launch out-of-box.
set(_dlss_dlls
    "${DLSS_DLL_REL_DIR}/nvngx_dlss.dll"
    "${DLSS_DLL_REL_DIR}/nvngx_dlssg.dll")
foreach(_dll IN LISTS _dlss_dlls)
    if(EXISTS "${_dll}")
        add_custom_command(TARGET dlss-compositor-cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:dlss-compositor-cli>"
            COMMENT "Copying DLSS runtime DLL: ${_dll}"
            VERBATIM)
    else()
        message(WARNING "DLSS runtime DLL missing (skipped copy): ${_dll}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# GLSL → SPIR-V helper (used by later shader tasks).
# -----------------------------------------------------------------------------
find_program(GLSLC_EXECUTABLE glslc
    HINTS
        ENV VULKAN_SDK
        ENV VK_SDK_PATH
    PATH_SUFFIXES Bin bin)
if(GLSLC_EXECUTABLE)
    message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")
else()
    message(STATUS "glslc not found; shader compilation will fail until Vulkan SDK is installed.")
endif()

# Compile a single .comp shader to .spv embedded into a target.
function(dlss_add_shader TARGET SHADER_SRC)
    if(NOT GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc not found — cannot compile shader ${SHADER_SRC}")
    endif()
    get_filename_component(_name ${SHADER_SRC} NAME)
    set(_spv "${CMAKE_BINARY_DIR}/shaders/${_name}.spv")
    add_custom_command(
        OUTPUT  "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
        COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O
                "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}" -o "${_spv}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}"
        COMMENT "GLSLC ${SHADER_SRC} -> ${_spv}"
        VERBATIM)
    target_sources(${TARGET} PRIVATE "${_spv}")
endfunction()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
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
