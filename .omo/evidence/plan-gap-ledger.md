# Plan Gap Ledger — 2026-06-04

Scope: `.omo/plans/dlss-compositor-v2.md` vs current branch after SR/FG CLI pipeline fixes. User decision overrides original native-plugin primary path: current intended GUI path is Flutter → CLI process, not in-process engine bridge.

## Resolved in this session

- Real Blender multipart EXR loading: `src/io/frame_prefetcher.cpp` reads RGBA, Z/Z.V, Vector.X/Y/Z/W and raw Vector X/Y/Z/W parts.
- SR-only real pipeline: user config `E:\New Folder(1)\dlssconfig.json` produces 23 EXR frames.
- FG 2x on RTX 4060: fixed `MultiFrameCountMax` interpretation and compiled real SPIR-V shaders; `--fg 2x` produces 45 EXR frames.
- Shader build: `glslang[tools]` added to vcpkg manifest, CMake compiles depth/MV/tonemap/inverse SPIR-V and copies all 4 `.spv` next to CLI.
- GUI output folder button: opens validated absolute folder path and reports missing/spawn failures.

## Remaining blockers / follow-up queue

### P0 — must resolve before final plan signoff

1. Blender test suite missing from workspace
   - Expected by plan: `blender/tests/conftest.py`, `test_register.py`, `test_quality_apply.py`, `test_jitter.py`, `test_handlers.py`, `test_dlssconfig_writer.py`, `test_integration.py`, `cross_check_constants.py`.
   - Current: only `MV_SANITY_RESULT.md` / cache artifacts observed.

2. Blender schema validation and bundled wheels incomplete
   - `blender/dlss_compositor/wheels/` is empty.
   - `blender_manifest.toml` lacks top-level `wheels = [...]` before sections.
   - `dlssconfig_writer.py` should validate against `src/core/dlssconfig.schema.json` before writing.

3. Non-EXR encoders are placeholders
   - `png_16` and `prores_422hq` paths exist in UI/CLI, but audit found encoder stubs/placeholders.
   - Either implement real PNG16/ProRes 422 HQ or hide/reject until complete.

### P1 — high priority correctness/compliance

4. RPC/native plugin engine path is superseded by current user decision but plan still lists it
   - Original plan wants native plugin linked to `dlss_engine_lib` and JSON-RPC fallback.
   - Current user decision: Flutter as test/UI entry, CLI backend process, no JSON-RPC primary path.
   - Action: update docs/plan/status to avoid treating this as an implementation blocker, or explicitly re-scope if in-process bridge is still desired later.

5. DLSS citation compliance audit still needed
   - Audit flagged NGX extension queries in `src/gpu/vulkan_context.cpp` and some NGX calls as needing strict same/preceding-line `// DLSS §...` comments.

6. `--self-check` remains incomplete
   - Plan expects real Vulkan/NGX capability reporting.
   - Current CLI config path creates Vulkan/NGX correctly, but `--self-check` still needs a real implementation.

7. Camera cut matrix discontinuity threshold needs finalization
   - Gap audit flagged SR-only threshold effectively disabled/infinite in current implementation.
   - Needs real threshold or documented rationale based on user fixtures.

## Verified commands after latest FG fixes

```text
cmake --build build/x64-windows-msvc-release --config Release --target dlss-compositor-cli dlss_cli_tests dlss_engine_tests
dlss_cli_tests.exe  -> passed
dlss_engine_tests.exe "FramePrefetcher: Blender multipart EXR decodes*" -> passed
dlss-compositor-cli.exe --config "E:\New Folder(1)\dlssconfig.json" --output-dir "E:\New Folder(1)\output-fg2" --output-template frame_####.exr --output-format exr --overwrite-mode overwrite --events stderr --fg 2x -> EXIT=0, 45 EXRs
flutter analyze -> passed
flutter test test/detail_panel_test.dart test/dlss_compositor_plugin_test.dart test/task_config_panel_test.dart -> passed
git diff --check -> passed
```
