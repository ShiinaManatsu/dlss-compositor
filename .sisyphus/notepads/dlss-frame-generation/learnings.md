# Learnings — dlss-frame-generation

## 2026-04-11 — Initial Session: ses_28485e441ffezka9bCSXgP2ZD3

### CMake Patterns
- `CMakeLists.txt:155-163`: DLL copy uses `file(GLOB ...)` + `foreach` loop with `add_custom_command POST_BUILD copy_if_different`
- `dlss_compositor_core` static lib (lines 100-128) contains all source files — new cpp files must be added here
- No nlohmann/json in CMakeLists.txt — need to add FetchContent for it or use a simple hand-rolled parser
- `tests/CMakeLists.txt:34-40`: Same DLL copy pattern for test target

### DLSS SDK Layout (already confirmed present)
- `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h` — NGX_VK_CREATE_DLSSG, NGX_VK_EVALUATE_DLSSG
- `DLSS/include/nvsdk_ngx_params_dlssg.h` — NVSDK_NGX_DLSSG_Create_Params, NVSDK_NGX_DLSSG_Opt_Eval_Params
- `DLSS/include/nvsdk_ngx_defs_dlssg.h` — NVSDK_NGX_Parameter_FrameGeneration_Available
- `DLSS/lib/Windows_x86_64/rel/nvngx_dlssg.dll` — DLL to copy (alongside nvngx_dlssd.dll)

### Build
- MSVC, C++17, cmake --build build --config Release
- build directory: `F:/Projects/GitRepos/dlss-compositor/build`
- Test binary: `build\Release\dlss-compositor-tests.exe`
- Release output now also copies `nvngx_dlssg.dll` alongside `nvngx_dlssd.dll` for both app and tests.
- Stub FG/camera files were added so CMake source registration stays buildable until the real implementations land.

### Key Constraints
- NO Streamline SDK — raw NGX API only
- NO modifications to DlssRRProcessor
- mvecScale for DLSS-G = [1/W, 1/H] — NOT 1.0 like DLSS-RR
- colorBuffersHDR=1 for scene-linear
- First frame: reset=true
- 4x MFG: call evaluate 3 times with multiFrameIndex=1,2,3

### CLI Notes
- `--interpolate` now parses `2x`/`4x` into `AppConfig::interpolateFactor`.
- `--camera-data` stores a camera metadata JSON path in `AppConfig::cameraDataFile`.
- Interpolation requests are rejected unless camera data is also provided.
