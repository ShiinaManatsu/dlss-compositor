cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)

# ── Bundle MSVC Redist DLLs so the exe runs on machines without Visual Studio ──
if(WIN32 AND MSVC)
  get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_cl_host_dir "${_cl_dir}" DIRECTORY)
  get_filename_component(_cl_bin_dir  "${_cl_host_dir}" DIRECTORY)
  get_filename_component(_cl_ver_dir  "${_cl_bin_dir}" DIRECTORY)
  get_filename_component(_msvc_dir    "${_cl_ver_dir}" DIRECTORY)
  get_filename_component(_tools_dir   "${_msvc_dir}" DIRECTORY)
  get_filename_component(_vc_dir      "${_tools_dir}" DIRECTORY)

  file(GLOB _crt_dirs LIST_DIRECTORIES true
    "${_vc_dir}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
  if(_crt_dirs)
    list(GET _crt_dirs 0 _crt_dir)
    foreach(_dll IN ITEMS msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
      add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_crt_dir}/${_dll}"
          "$<TARGET_FILE_DIR:${BINARY_NAME}>"
        COMMENT "Bundling ${_dll}"
        VERBATIM)
    endforeach()

    get_filename_component(_ver_dir "${_crt_dir}/../.." ABSOLUTE)
    file(GLOB _dbg_dirs LIST_DIRECTORIES true
      "${_ver_dir}/debug_nonredist/x64/Microsoft.VC*.DebugCRT")
    if(_dbg_dirs)
      list(GET _dbg_dirs 0 _dbg_dir)
      foreach(_dll IN ITEMS msvcp140d.dll vcruntime140d.dll vcruntime140_1d.dll)
        add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dbg_dir}/${_dll}"
            "$<TARGET_FILE_DIR:${BINARY_NAME}>"
          COMMENT "Bundling ${_dll}"
          VERBATIM)
      endforeach()
    endif()

    file(GLOB _ucrt_dirs LIST_DIRECTORIES true
      "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/ucrt")
    if(_ucrt_dirs)
      list(GET _ucrt_dirs 0 _ucrt_dir)
      add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_ucrt_dir}/ucrtbased.dll"
          "$<TARGET_FILE_DIR:${BINARY_NAME}>"
        COMMENT "Bundling ucrtbased.dll"
        VERBATIM)
    endif()
  endif()
endif()
