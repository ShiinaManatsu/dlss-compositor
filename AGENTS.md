# AGENTS.md — DLSS Compositor v2

High-signal notes for future OpenCode sessions. Keep this file compact; prefer verified build/config facts over plans.

## Repository shape

- `cli/src/`: C++23 Vulkan/NGX engine, `dlss_engine_lib`, and `dlss-compositor-cli.exe`.
- `cli/resources/`: engine-side reference assets; `legacy-luts/` is archival scratch data, not the active Blender-export LUT path.
- `app/`: Electron + React Spectrum 2 product UI — rail navigation (项目/导出/Jobs/NR 工具/设置). Pre-redesign V3 UI kept read-only under `app/ref-v3ui/` (excluded from tsconfig/vite). CLI subprocess is the backend; do not add a native in-process plugin path. Packaging: electron-builder (`npm run dist`), backend bundle assembled by `app/scripts/pack-backend.mjs`.
- `blender-plugin/`: Blender 5.1+ extension that writes multilayer EXR input and `dlssconfig.json`.
- `docs/dlss-refs/`: local DLSS SDK doc mirrors; cite these before changing NGX calls.
- `docs/research/`: scratch notes worth keeping around but not treating as source-of-truth docs.
- `docs/external/`: large third-party reference drops kept outside the active module roots.
- `DLSS/`: NVIDIA DLSS SDK v310.9.16 submodule (headers + import libs). Initialize submodules before configuring CMake. See `docs/dlss-refs/CHANGELOG-310.9.16.md` for upgrade notes.
- DLSS feature DLLs (310.9.16.0, from the SDK's `lib/Windows_x86_64/rel/`) ship with the app, next to the exe (DLSS Programming Guide §5.1): CMake POST_BUILD copies `resources/nvngx_dlss.dll` / `nvngx_dlssd.dll` / `nvngx_dlssg.dll`, and the release zip bundles them the same way. NGX core (`_nvngx.dll`) always comes from the NVIDIA driver — never bundle it.
- DLSSNR (feature 18) uses a separate signed `nvngx_dlssnr.dll`, loaded by `cli/src/dlss/dlss_nr.cpp`. The local RTX 50 build uses NVIDIA's original 310.9.16.0 from Magpie v0.6.11's optional archive. `nr_model`: 0 Auto (runtime default), 1 Standard A, 2 Lightweight B (310.9.16+); 310.8 Auto selects A. Model changes recreate the feature. Keep the DLL local and gitignored; `pack-backend.mjs` packages SR/RR/FG only.
- NGX DLSS RR has no UltraQuality slot: the OptimalSettings callback silently no-ops (success, all-zero range) for `PerfQuality_UltraQuality`, and UltraPerformance accepts only an exact 1/3 render scale. Valid RR quality slots: DLAA / Quality / Balanced / Performance. CLI dispatcher rejects `ultra_quality` for RR; the GUI RR quality picker filters to `RR_QUALITY_IDS` (dcp.ts) and the orchestrator re-checks at start(). NGX range floors are CEILed for odd sizes (1215→608) — mirror in `ngx_min_render_size` (session_config.py) and `ngxMinSize` (appState).
- Root should stay lean: module roots, build entry files, scripts, and top-level docs only.

## Build and verification flow

- Requirements: Windows + MSVC, CMake 3.25+ for presets, `VCPKG_ROOT`, Node.js + npm (for `app/`), DLSS submodule/LFS files, and either Vulkan SDK `glslc` or vcpkg `glslang[tools]`.
- CMake presets use Ninja Multi-Config and vcpkg triplet `x64-windows-static-md`.
- The maintainer removed all automated test suites (native, Python, UI, smoke, end-to-end, and benchmarks). Do not add or restore tests unless explicitly requested. Verify changes with builds and task-specific manual runtime inspection.

```powershell
# Normal local configure/build. Without -B, preset output is build/x64-windows-msvc-release.
cmake --preset x64-windows-msvc-release
cmake --build --preset x64-windows-msvc-release --parallel

# CI-style equivalent writes to build/ instead.
git submodule update --init --recursive
git lfs pull
cmake -B build --preset x64-windows-msvc-release -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release --parallel

# Focused native builds used during iteration.
cmake --build build/x64-windows-msvc-release --config Release --target dlss-compositor-cli
cmake --build build/x64-windows-msvc-release --config Release --target dlss_engine_lib

# If plain cmake/MSVC env is missing, run inside vcvars64 or use VS bundled CMake:
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "F:\Projects\GitRepos\dlss-compositor\build\x64-windows-msvc-release" --config Release --target dlss-compositor-cli'
```

```powershell
# Electron GUI (app/)
cd app
npm install
npm run build        # tsc --noEmit + vite build + esbuild main bundle

# Dev loop (vite :5173 + electron attach)
npm run dev
```

- ProRes packets must carry per-frame durations: `prores_writer.cpp` stamps `packet->duration = 1` (codec time_base) because the encoders don't propagate `AVFrame.duration` — without it the mov's stream duration ends at the last PTS and avformat derives a bogus `avg_frame_rate`, which breaks `VideoReader`'s CFR index math ("skipped past frame N"). VideoReader prefers `r_frame_rate` when `avg_frame_rate` disagrees by >1% as defense against foreign files.
- `sws_setColorspaceDetails` takes 16.16 fixed-point contrast/saturation: neutral is `1 << 16`, and `0` zeroes the image (every decoded frame comes out black). `VideoReader::readFrameRgba8` was shipped with `0, 0, 0` once and the NR tool previewed/exported pure black for weeks. Inspect decoded content when validating video output; file byte counts do not establish correct pixels.
- `nr_buffer_dir` holds `dlssnrbuffer.mkv` (FFV1 gray16 panels: MV.x, MV.y, NDC depth) and the GUI's `dlssconfig.json`. Motion is rounded to 1/32 px; FP16 depth is unchanged. NR/FG still reads legacy `dlssnrbuffer_*.exr`. FG must be off while exporting the guide video. Rolling jobs write one guide video in frame order.

- `dlss-compositor-cli --self-check` is not yet a real capability probe; trust `NgxContext`/status logs and manual runtime inspection instead.
- Stage 3 (`pipeline/run.py`) talks to the CLI over `--rpc-stdio` JSON-RPC. That path is verified.
- Rolling parallel render (`app/electron/rollingRender.ts`) assigns windows to persistent Blender producers. Each producer atomically publishes complete EXRs to a shared folder. One CLI consumes and deletes frames in order, keeping one NGX history and one muxer for the full timeline. Reserve uncompressed float32 passes at actual render size plus `max(1 GiB, 2 frames)` of headroom: 8 GiB at 200 MiB/frame allows 35 outstanding frames, or windows of up to 17 with two workers. Bake stateful simulations before using multiple workers. SR Beauty + Guides stays on the original path.
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

- Blender compositor API names matter:
  - Blender 5.3+ uses `scene.compositor_effects`: add a separate pass-through effect for File Output. `scene.compositing_node_group` is the older API; in 5.3 it resolves only the active effect.
  - `scene.render.use_compositing`, not `scene.use_nodes`.
  - File Output uses `directory`, `file_name`, and `file_output_items`; create items with socket types like `"FLOAT"`.
  - File Output format is `OPEN_EXR_MULTILAYER`.
  - Runtime wants `view_transform = "Raw"` even when stubs imply `"NONE"`.
- Cycles 5.3 needs both `denoising_store_passes` and `denoising_store_motion_passes` for the RR guide set. Motion blur suppresses the denoising motion passes. The EXR reader must check parts after the mandatory albedo/normal/roughness parts for reflection motion.
- All `bpy.app.handlers.*` callbacks must have `@bpy.app.handlers.persistent` or Blender drops them on file load.
- If adding bundled Blender wheels, list `wheels = [...]` before any `[section]` in `blender_manifest.toml`; Blender 5.1 uses Python 3.13 (`cp313`) for native wheels.

## False claims to avoid

- GUI is Electron + React Spectrum 2 (`app/`): rail nav (项目/导出/Jobs/NR / FG/设置), breadcrumb title bar with route-history Back and theme utility; 导出/停止 in the title bar, progress overlays the preview. The rail uses React Aria Links + S2 macros and Provider routing (not ToggleButtonGroup or internal-slot overrides). Workbench inspectors move to an S2 Dialog below 920px content width / 420px height, retaining controlled edits. Shell gutters/padding come from macro variables (16/20). Product CJK fallback and dev selector processing live in `app/scripts/vite-s2-*.ts`; revalidate on S2 upgrades. See `docs/research/gui-s2-review-2026-09-30.md`. `app/ref-v3ui/` is the retired V3 shell kept for reference only.
- Not DX12 output: v2 is Vulkan only.
- Not NIS: SR is DLSS only.
- Not general video export yet: ProRes 422 HQ is the only implemented video muxer; ProRes 4444 and other video codecs are not. The NR tool exports ProRes 422 HQ / H.265 / PNG16 only.
- FG does not “clamp HDR to 10 nits”; current HDR transport is scene-linear -> display-ready for FG, then inverse for EXR output.
