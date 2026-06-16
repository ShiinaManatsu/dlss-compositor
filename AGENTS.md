# AGENTS.md — DLSS Compositor v2

High-signal notes for future OpenCode sessions. Keep this file compact; prefer verified build/config facts over plans.

## Repository shape

- `dlss-plugin/src/`: C++23 Vulkan/NGX engine, `dlss_engine_lib`, and `dlss-compositor-cli.exe`.
- `dlss-plugin/tests/`: native Catch2 coverage and focused regression checks.
- `dlss-plugin/resources/`: engine-side reference assets; `legacy-luts/` is archival scratch data, not the active Blender-export LUT path.
- `gui/`: Flutter Windows GUI. Default backend is the native Windows plugin; CLI subprocess is only a debug fallback via `DLSS_COMPOSITOR_GUI_BACKEND=cli`.
- `blender-plugin/`: Blender 5.1+ extension that writes multilayer EXR input and `dlssconfig.json`.
- `docs/dlss-refs/`: local DLSS SDK doc mirrors; cite these before changing NGX calls.
- `docs/research/`: scratch notes worth keeping around but not treating as source-of-truth docs.
- `docs/external/`: large third-party reference drops kept outside the active module roots.
- `DLSS/`: NVIDIA DLSS SDK v310.6.0 submodule. Initialize submodules before configuring CMake.
- Root should stay lean: module roots, build entry files, scripts, and top-level docs only.

## Build and verification flow

- Requirements: Windows + MSVC, CMake 3.25+ for presets, `VCPKG_ROOT`, Flutter, DLSS submodule/LFS files, and either Vulkan SDK `glslc` or vcpkg `glslang[tools]`.
- CMake presets use Ninja Multi-Config and vcpkg triplet `x64-windows-static-md`.

```powershell
# Normal local configure/build/test. Without -B, preset output is build/x64-windows-msvc-release.
cmake --preset x64-windows-msvc-release
cmake --build --preset x64-windows-msvc-release --parallel
ctest --preset x64-windows-msvc-release --output-on-failure

# CI-style equivalent writes to build/ instead.
git submodule update --init --recursive
git lfs pull
cmake -B build --preset x64-windows-msvc-release -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release --parallel
ctest --test-dir build --config Release --output-on-failure

# Focused native builds used during iteration.
cmake --build build/x64-windows-msvc-release --config Release --target dlss-compositor-cli dlss_engine_tests dlss_cli_tests
cmake --build build/x64-windows-msvc-release --config Release --target dlss_engine_lib

# If plain cmake/MSVC env is missing, run inside vcvars64 or use VS bundled CMake:
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "D:\Github\dlss-compositor-opus-rework\build\x64-windows-msvc-release" --config Release --target dlss-compositor-cli dlss_engine_tests dlss_cli_tests'
```

```powershell
# Flutter GUI
cd gui
flutter pub get
flutter analyze
flutter test
flutter build windows --release

# GUI release output and local packaging. The built app directory is self-contained;
# zip the directory contents or copy it into a release bundle as `gui/`.
# This stages under `build/release/` and writes final zip artifacts to the repo root by default.
powershell -ExecutionPolicy Bypass -File ../scripts/package_release.ps1

# Useful focused GUI checks
flutter test test/task_config_panel_test.dart test/dlss_compositor_plugin_test.dart
```

- GPU/NGX tests are tagged `[gpu]` and require NVIDIA RTX + drivers; run locally with `ctest -C Release --label-regex gpu` from the configured build dir.
- `dlss-compositor-cli --self-check` is not yet a real capability probe; trust `NgxContext`/status logs and targeted tests instead.
- Do not rely on `--rpc-stdio`, `-DBUILD_TESTS=OFF`, or `python -m pytest blender-plugin/tests/` as verified flows; current executable config does not support them as documented.
- For clangd, generate or expose `compile_commands.json`; otherwise diagnostics miss MSVC/vcpkg include paths and C++23 flags.

## DLSS / Vulkan invariants

- Every `NVSDK_NGX_*` API call needs a nearby `// DLSS §...` citation comment. Use `docs/dlss-refs/` for section numbers.
- stdout is reserved for JSON-RPC `Content-Length` frames. Diagnostics go through `spdlog` to stderr + rotating log files. Do not add `std::cout`, `printf`, or `fprintf(stdout, ...)` in `dlss-plugin/src/` except the RPC transport itself.
- Do not allocate GPU images per frame. Use `TexturePool::acquire/release`; per-frame `vkCreateImage` is a regression.
- Motion vectors are pixel-space curr→prev. Keep SR `InMVScaleX/Y = 1.0`; FG `mvecScale` follows the DLSSG SDK header and normalizes pixel-unit MVs by the motion-vector resource size.
- Current Blender MV mapping is `DLSS_MV = (Blender.X, -Blender.Y)`. Do not whole-vector negate.
- `InReset` is only for first frame, frame gaps, or camera cuts; never set it every frame.
- SR input is scene-linear before tonemap. FG input is display-ready after tonemap/EOTF.
- Guide-buffer dumps are gated by `DLSS_COMPOSITOR_DUMP_GUIDES=1` and write under the output dir.

## Output formats and schema

- Implemented outputs today: EXR scene-linear and PNG16 for LDR. HDR + PNG is rejected.
- ProRes 422 HQ is implemented as a sequence-level MOV muxer via `ProResWriter`; it is intentionally not registered in the per-frame `EncoderRegistry`. LDR uses BT.709 tagging; HDR uses HDR10/PQ + BT.2020 tagging for display-ready review/export. DNxHR/H264/H265/AV1/AVIF/JPEG/WebP and ProRes 4444 remain placeholders and must not be exposed.
- `dlss-plugin/src/core/dlssconfig.schema.json` is the single schema source; do not copy it into `gui/` or `blender-plugin/`.
- GUI template text is stored extensionless (`frame_####`); selected output format appends/replaces `.png` or `.exr` when launching the task.

## GUI/runtime/logging gotchas

- Native plugin owns a resident `VulkanContext` + `NgxContext`; `checkReady`, `getStatus`, and `startTask` should reuse that runtime. Avoid one-off NGX status probes.
- GUI/native logs go beside the GUI exe under `logs/`; native C++ honors `DLSS_COMPOSITOR_LOG_DIR`. Debug runner sets `DLSS_COMPOSITOR_LOG=debug` and `DLSS_COMPOSITOR_GUI_LOG=1` if missing.
- Release staging artifacts belong under `build/release/` and are ignored. Do not commit generated `.omo/evidence/`, `build/release/`, repo-root release zips, or `.omo/run-continuation/` artifacts.

## Blender extension gotchas

- Blender 5.1 API names matter:
  - `scene.compositing_node_group`, not `scene.node_tree`.
  - `scene.render.use_compositing`, not `scene.use_nodes`.
  - File Output uses `directory`, `file_name`, and `file_output_items`; create items with socket types like `"FLOAT"`.
  - File Output format is `OPEN_EXR_MULTILAYER`.
  - Runtime wants `view_transform = "Raw"` even when stubs imply `"NONE"`.
- All `bpy.app.handlers.*` callbacks must have `@bpy.app.handlers.persistent` or Blender drops them on file load.
- If adding bundled Blender wheels, list `wheels = [...]` before any `[section]` in `blender_manifest.toml`; Blender 5.1 uses Python 3.13 (`cp313`) for native wheels.

## False claims to avoid

- Not Electron: GUI is Flutter on Windows.
- Not DX12 output: v2 is Vulkan only.
- Not NIS: SR is DLSS only.
- Not general video export yet: ProRes 422 HQ is the only implemented video muxer; ProRes 4444 and other video codecs are not.
- FG does not “clamp HDR to 10 nits”; current HDR transport is scene-linear -> display-ready for FG, then inverse for EXR output.
