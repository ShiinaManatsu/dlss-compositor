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
target_link_libraries(${PROJECT_NAME}_plugin PRIVATE flutter flutter_wrapper_plugin)

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(dlss_compositor_plugin_bundled_libraries
  # Defined in ../src/CMakeLists.txt.
  # This can be changed to accommodate different builds.
  $<TARGET_FILE:dlss_compositor_plugin>
  PARENT_SCOPE
)
