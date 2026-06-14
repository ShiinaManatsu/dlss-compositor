# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

# Project-level configuration.
set(PROJECT_NAME "dlss_compositor_plugin")
project(${PROJECT_NAME} LANGUAGES C CXX)

# Invoke the build for native FFI code shared with other target platforms.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")

get_filename_component(_DLSS_CMAKE_SOURCE_REAL "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(_DLSS_CURRENT_SOURCE_REAL "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
set(_DLSS_COMPOSITOR_ROOT_CANDIDATES
  "${_DLSS_CMAKE_SOURCE_REAL}/../.."
  "${_DLSS_CURRENT_SOURCE_REAL}/../../.."
)
foreach(_DLSS_COMPOSITOR_ROOT_CANDIDATE IN LISTS _DLSS_COMPOSITOR_ROOT_CANDIDATES)
  get_filename_component(_DLSS_COMPOSITOR_ROOT_ABS
    "${_DLSS_COMPOSITOR_ROOT_CANDIDATE}" ABSOLUTE)
  if(EXISTS "${_DLSS_COMPOSITOR_ROOT_ABS}/CMakeLists.txt"
      AND EXISTS "${_DLSS_COMPOSITOR_ROOT_ABS}/vcpkg.json")
    set(DLSS_COMPOSITOR_ROOT "${_DLSS_COMPOSITOR_ROOT_ABS}" CACHE PATH "DLSS Compositor root")
    break()
  endif()
endforeach()
if(NOT DEFINED DLSS_COMPOSITOR_ROOT)
  message(FATAL_ERROR "Could not locate DLSS Compositor root")
endif()
if(NOT TARGET dlss_engine_lib)
  add_subdirectory("${DLSS_COMPOSITOR_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/engine")
endif()

# Native Windows plugin registrar target. Flutter links this target into the
# runner when pluginClass is declared in pubspec.yaml.
add_library(${PROJECT_NAME}_plugin STATIC
  "dlss_compositor_plugin.cpp"
)

apply_standard_settings(${PROJECT_NAME}_plugin)
set_target_properties(${PROJECT_NAME}_plugin PROPERTIES
  CXX_VISIBILITY_PRESET hidden
)
target_compile_definitions(${PROJECT_NAME}_plugin PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PROJECT_NAME}_plugin INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(${PROJECT_NAME}_plugin PRIVATE
  "${DLSS_COMPOSITOR_ROOT}/src"
)
target_link_libraries(${PROJECT_NAME}_plugin PRIVATE flutter flutter_wrapper_plugin dlss_engine_lib)

if(WIN32 AND DEFINED DLSS_SDK_ROOT)
  set(_dlss_runtime_dir "${DLSS_SDK_ROOT}/lib/Windows_x86_64/rel")
  set(_dlss_runtime_dlls)
  foreach(_dlss_dll IN ITEMS nvngx_dlss.dll nvngx_dlssg.dll)
    if(EXISTS "${_dlss_runtime_dir}/${_dlss_dll}")
      list(APPEND _dlss_runtime_dlls "${_dlss_runtime_dir}/${_dlss_dll}")
      add_custom_command(TARGET ${PROJECT_NAME}_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_dlss_runtime_dir}/${_dlss_dll}"
          "$<TARGET_FILE_DIR:${PROJECT_NAME}_plugin>"
        COMMENT "Copying DLSS runtime DLL for Flutter plugin: ${_dlss_dll}"
        VERBATIM)
    endif()
  endforeach()
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(dlss_compositor_plugin_bundled_libraries
  # Defined in ../src/CMakeLists.txt.
  # This can be changed to accommodate different builds.
  $<TARGET_FILE:dlss_compositor_plugin>
  ${_dlss_runtime_dlls}
  PARENT_SCOPE
)
