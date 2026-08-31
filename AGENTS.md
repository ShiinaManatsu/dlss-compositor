# AGENTS.md — DLSS Compositor v2

High-signal notes for future OpenCode sessions. Keep this file compact; prefer verified build/config facts over plans.

## Repository shape

- `cli/src/`: C++23 Vulkan/NGX engine, `dlss_engine_lib`, and `dlss-compositor-cli.exe`.
- `cli/tests/`: native Catch2 coverage and focused regression checks.
- `cli/resources/`: engine-side reference assets; `legacy-luts/` is archival scratch data, not the active Blender-export LUT path.
- `app/`: Electron + React Spectrum 2 product UI — rail navigation (项目/导出/Jobs/NR 工具/设置). Pre-redesign V3 UI kept read-only under `app/ref-v3ui/` (excluded from tsconfig/vite). CLI subprocess is the backend; do not add a native in-process plugin path. Packaging: electron-builder (`npm run dist`), backend bundle assembled by `app/scripts/pack-backend.mjs`.
- `blender-plugin/`: Blender 5.1+ extension that writes multilayer EXR input and `dlssconfig.json`.
- `docs/dlss-refs/`: local DLSS SDK doc mirrors; cite these before changing NGX calls.
- `docs/research/`: scratch notes worth keeping around but not treating as source-of-truth docs.
- `docs/external/`: large third-party reference drops kept outside the active module roots.
- `DLSS/`: NVIDIA DLSS SDK v310.6.0 submodule (headers + import libs). Initialize submodules before configuring CMake.
- DLSS feature DLLs (310.7.129.0) ship with the app, next to the exe (DLSS Programming Guide §5.1): CMake POST_BUILD copies `resources/nvngx_dlss.dll` / `nvngx_dlssd.dll` / `nvngx_dlssg.dll` for Debug, and the release zip bundles them the same way. NGX core (`_nvngx.dll`) always comes from the NVIDIA driver — never bundle it.
- DLSSNR (feature 18) is a signed snippet (`nvngx_dlssnr.dll` 310.8.0.0) loaded directly by `cli/src/dlss/dlss_nr.cpp` — the core DLL does not route it. The snippet verifies its caller module, so `DlssNrPass` IAT-hooks the snippet's `GetModuleFileNameW` import to answer `nvngx.dll` (Magpie approach) or Init fails with 0xBAD00002. That DLL is dev-machine-only: gitignored under `resources/`, and `pack-backend.mjs` keeps its fixed 3-DLL whitelist (installers must not ship it).
- NGX DLSS RR has no UltraQuality slot: the OptimalSettings callback silently no-ops (success, all-zero range) for `PerfQuality_UltraQuality`, and UltraPerformance accepts only an exact 1/3 render scale. Valid RR quality slots: DLAA / Quality / Balanced / Performance. CLI dispatcher rejects `ultra_quality` for RR; the GUI RR quality picker filters to `RR_QUALITY_IDS` (dcp.ts) and the orchestrator re-checks at start(). NGX range floors are CEILed for odd sizes (1215→608) — mirror in `ngx_min_render_size` (session_config.py) and `ngxMinSize` (appState).
- Root should stay lean: module roots, build entry files, scripts, and top-level docs only.

## Build and verification flow

- Requirements: Windows + MSVC, CMake 3.25+ for presets, `VCPKG_ROOT`, Node.js + npm (for `app/`), DLSS submodule/LFS files, and either Vulkan SDK `glslc` or vcpkg `glslang[tools]`.
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
# Electron GUI (app/)
cd app
npm install
npm run build        # tsc --noEmit + vite build + esbuild main bundle
npm run test:progress; npm run test:dcp; npm run test:errors

# Dev loop (vite :5173 + electron attach)
npm run dev

# Headless self-test screenshots (offscreen capture)
npx electron . --home --screenshot ../build/shot-project.png --quit-on-shot
npx electron . --dcp <x.dcp> --screenshot ../build/shot-export.png --quit-on-shot
npx electron . --home --page jobs --screenshot ../build/shot-jobs.png --quit-on-shot
npx electron . --home --page settings --screenshot ../build/shot-settings.png --quit-on-shot
npx electron . --home --page nrtool --screenshot ../build/shot-nrtool.png --quit-on-shot
npx electron . --dcp <x.dcp> --demo-progress --screenshot ../build/shot-run.png --quit-on-shot
```

- NR standalone workflow regression: `python tools/run_nr_tool_smoke.py` (needs Stage-A exports in `test-frames/rr/pipeline-out-nrbuf` — color PNG16 + `dlssnrbuffer_*.exr`; regenerable via `tools/run_nr_buffer_job.py`). Exercises nrtool_open → preview/scrub → ProRes export → close over JSON-RPC.
- Full R:→S: disk flow regression: `python tools/run_e2e_rs_smoke.py [--stages abc]` — seeds `test-frames/rr` into the RAM disk, then RR+nr_buffer watch export (ProRes + dlssnrbuffer + preview_frame) → NR tool consume/export → SR watch export. Stage A consumes input frames; the tool reseeds before C. Requires the RAM disks mounted (R:, S:).
- ProRes packets must carry per-frame durations: `prores_writer.cpp` stamps `packet->duration = 1` (codec time_base) because the encoders don't propagate `AVFrame.duration` — without it the mov's stream duration ends at the last PTS and avformat derives a bogus `avg_frame_rate`, which breaks `VideoReader`'s CFR index math ("skipped past frame N"). VideoReader prefers `r_frame_rate` when `avg_frame_rate` disagrees by >1% as defense against foreign files.

- GPU/NGX tests are tagged `[gpu]` and require NVIDIA RTX + drivers; run locally with `ctest -C Release --label-regex gpu` from the configured build dir.
- `dlss-compositor-cli --self-check` is not yet a real capability probe; trust `NgxContext`/status logs and targeted tests instead.
- Stage 3 (`pipeline/run.py`) talks to the CLI over `--rpc-stdio` JSON-RPC. That path is verified. `-DBUILD_TESTS=OFF` and `python -m pytest blender-plugin/tests/` are still not verified flows.
- For clangd, generate or expose `compile_commands.json`; otherwise diagnostics miss MSVC/vcpkg include paths and C++23 flags.

## DLSS / Vulkan invariants

- Every `NVSDK_NGX_*` API call needs a nearby `// DLSS §...` citation comment. Use `docs/dlss-refs/` for section numbers.
- stdout is reserved for JSON-RPC `Content-Length` frames. Diagnostics go through `spdlog` to stderr + rotating log files. Do not add `std::cout`, `printf`, or `fprintf(stdout, ...)` in `cli/src/` except the RPC transport itself.
- Do not allocate GPU images per frame. Use `TexturePool::acquire/release`; per-frame `vkCreateImage` is a regression.
- Motion vectors are pixel-space curr→prev. Keep SR `InMVScaleX/Y = 1.0`; FG `mvecScale` follows the DLSSG SDK header and normalizes pixel-unit MVs by the motion-vector resource size.
- Current Blender MV mapping is `DLSS_MV = (Blender.X, -Blender.Y)`. Do not whole-vector negate.
- `InReset` is only for first frame, frame gaps, or camera cuts; never set it every frame.
- SR input is scene-linear before tonemap. FG input is display-ready after tonemap/EOTF.
- NR tool sessions and pipeline jobs are mutually exclusive by design (dispatcher rejects nrtool_open/preview during a job and start_job during an NR export); `stop` cancels whichever is running. NR consumes display-ready color only — scene-linear EXR is rejected as a color source.
- Guide-buffer dumps are gated by `DLSS_COMPOSITOR_DUMP_GUIDES=1` and write under the output dir.

## Output formats and schema

- Implemented outputs today: EXR scene-linear and PNG16 for LDR. HDR + PNG is rejected.
- ProRes 422 HQ is implemented as a sequence-level MOV muxer via `ProResWriter`; it is intentionally not registered in the per-frame `EncoderRegistry`. LDR uses BT.709 tagging; HDR uses HDR10/PQ + BT.2020 tagging for display-ready review/export. DNxHR/H264/H265/AV1/AVIF/JPEG/WebP and ProRes 4444 remain placeholders and must not be exposed.
- `cli/src/core/dlssconfig.schema.json` is the single schema source; do not copy it into `app/` or `blender-plugin/`.
- GUI template text is stored extensionless (`frame_####`); selected output format appends/replaces `.png` or `.exr` when launching the task.

## GUI/runtime/logging gotchas

- Native plugin owns a resident `VulkanContext` + `NgxContext`; `checkReady`, `getStatus`, and `startTask` should reuse that runtime. Avoid one-off NGX status probes.
- GUI/native logs go beside the GUI exe under `logs/`; native C++ honors `DLSS_COMPOSITOR_LOG_DIR`. Debug runner sets `DLSS_COMPOSITOR_LOG=debug` and `DLSS_COMPOSITOR_GUI_LOG=1` if missing.
- Release staging artifacts belong under `build/release/` and are ignored. Do not commit `build/release/` or repo-root release zips.

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

- GUI is Electron + React Spectrum 2 (`app/`): rail nav (项目/导出/Jobs/NR 工具/设置)， breadcrumb title bar, Start/停止 in title bar on the Export page; progress overlays the preview. S2 usage follows the component docs (subpath imports for ActionMenu/MenuItem, ToggleButtonGroup for the rail, Disclosure groups in settings). `app/ref-v3ui/` is the retired V3 shell kept for reference only.
- Not DX12 output: v2 is Vulkan only.
- Not NIS: SR is DLSS only.
- Not general video export yet: ProRes 422 HQ is the only implemented video muxer; ProRes 4444 and other video codecs are not. The NR tool exports ProRes 422 HQ / H.265 / PNG16 only.
- FG does not “clamp HDR to 10 nits”; current HDR transport is scene-linear -> display-ready for FG, then inverse for EXR output.
