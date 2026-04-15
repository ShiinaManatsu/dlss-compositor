cmake_minimum_required(VERSION 3.20)
project(dlss-compositor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# DLSS SDK root (provided by user)
set(DLSS_SDK_ROOT "" CACHE PATH "Path to DLSS SDK (contains include/ and lib/)")
if(DLSS_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR "DLSS_SDK_ROOT must be set to the DLSS SDK directory")
endif()

# Find Vulkan
find_package(Vulkan REQUIRED)

# FetchContent for third-party deps
include(FetchContent)

# OpenEXR
FetchContent_Declare(
    openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG        v3.3.3
    GIT_SHALLOW    TRUE
)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(openexr)

# Catch2
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)

# volk (Vulkan loader)
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        1.3.295
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(volk)

# VulkanMemoryAllocator
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# GLFW (for ImGui window)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# nlohmann/json (header-only JSON library)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ImGui as a library (Vulkan + GLFW backend)
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(imgui_lib PUBLIC volk glfw)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# Core CPU image IO helpers
add_library(dlss_compositor_core STATIC
    src/core/exr_reader.cpp
    src/core/exr_writer.cpp
    src/core/channel_mapper.cpp
    src/core/mv_converter.cpp
    src/core/camera_data_loader.cpp
    src/dlss/dlss_sr_processor.cpp
    src/dlss/dlss_fg_processor.cpp
    src/dlss/ngx_wrapper.cpp
    src/gpu/vulkan_context.cpp
    src/gpu/texture_pipeline.cpp
    src/gpu/texture_pool.cpp
    src/gpu/lut_processor.cpp
    src/gpu/pq_transfer_processor.cpp
    src/pipeline/sequence_processor.cpp
    src/pipeline/frame_prefetcher.cpp
    src/pipeline/async_exr_writer.cpp
)
target_include_directories(dlss_compositor_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    src/
    ${DLSS_SDK_ROOT}/include
    ${VulkanMemoryAllocator_SOURCE_DIR}/include
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_link_libraries(dlss_compositor_core PUBLIC
    OpenEXR::OpenEXR
    volk
    glfw
    nlohmann_json::nlohmann_json
    $<$<CONFIG:Debug>:${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib>
    $<$<NOT:$<CONFIG:Debug>>:${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>
)
target_compile_definitions(dlss_compositor_core PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Main executable
add_executable(dlss-compositor src/main.cpp)
target_sources(dlss-compositor PRIVATE
    src/cli/cli_parser.cpp
    src/ui/app.cpp
    src/ui/image_viewer.cpp
    src/ui/settings_panel.cpp
)
target_include_directories(dlss-compositor PRIVATE
    src/
    ${DLSS_SDK_ROOT}/include
    ${VulkanMemoryAllocator_SOURCE_DIR}/include
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)
target_link_libraries(dlss-compositor PRIVATE
    volk
    imgui_lib
    glfw
    dlss_compositor_core
)
target_compile_definitions(dlss-compositor PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Copy DLSS DLL/lib to output directory
if(MSVC)
    file(GLOB DLSS_NGXCORE_DLLS "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
    foreach(DLL ${DLSS_NGXCORE_DLLS})
        add_custom_command(TARGET dlss-compositor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:dlss-compositor>
        )
    endforeach()
    file(GLOB DLSS_NGXCORE_FG_DLLS "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")
    foreach(DLL ${DLSS_NGXCORE_FG_DLLS})
        add_custom_command(TARGET dlss-compositor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" $<TARGET_FILE_DIR:dlss-compositor>
        )
    endforeach()
endif()

# Compile GLSL compute shaders to SPIR-V
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK or set VULKAN_SDK.")
endif()

set(SHADER_LUT_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/apply_lut.comp")
set(SHADER_LUT_OUTPUT "${CMAKE_BINARY_DIR}/shaders/apply_lut.comp.spv")
set(SHADER_PQ_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/pq_transfer.comp")
set(SHADER_PQ_OUTPUT "${CMAKE_BINARY_DIR}/shaders/pq_transfer.comp.spv")

add_custom_command(
    OUTPUT "${SHADER_LUT_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND ${GLSLC} "${SHADER_LUT_SOURCE}" -o "${SHADER_LUT_OUTPUT}"
    DEPENDS "${SHADER_LUT_SOURCE}"
    COMMENT "Compiling apply_lut.comp -> SPIR-V"
)
add_custom_command(
    OUTPUT "${SHADER_PQ_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND ${GLSLC} "${SHADER_PQ_SOURCE}" -o "${SHADER_PQ_OUTPUT}"
    DEPENDS "${SHADER_PQ_SOURCE}"
    COMMENT "Compiling pq_transfer.comp -> SPIR-V"
)
add_custom_target(compile_shaders DEPENDS "${SHADER_LUT_OUTPUT}" "${SHADER_PQ_OUTPUT}")
add_dependencies(dlss-compositor compile_shaders)

# Copy compiled shaders and default LUT files to output directory
add_custom_command(TARGET dlss-compositor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dlss-compositor>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SHADER_LUT_OUTPUT}" "$<TARGET_FILE_DIR:dlss-compositor>/shaders/apply_lut.comp.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SHADER_PQ_OUTPUT}" "$<TARGET_FILE_DIR:dlss-compositor>/shaders/pq_transfer.comp.spv"
)

# Copy LUT binaries if they exist (generated by tools/generate_aces2_lut.py)
set(LUT_DIR "${CMAKE_SOURCE_DIR}/luts")
if(EXISTS "${LUT_DIR}")
    add_custom_command(TARGET dlss-compositor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:dlss-compositor>/luts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LUT_DIR}" "$<TARGET_FILE_DIR:dlss-compositor>/luts"
        COMMENT "Copying LUT binaries to output directory"
    )
endif()

# Tests
enable_testing()
add_subdirectory(tests)
