# Learnings — rr-to-sr-refactor

## 2026-04-15 — Session ses_26eaf744cffeWPi53ue6j3Fq8a (Atlas)

### Project Structure
- C++ project with CMake build system
- Vulkan/NGX DLSS SDK integration
- Electron GUI frontend (TypeScript/React)
- Blender Python addon
- LSP errors for missing includes are PRE-EXISTING (volk.h, etc.) — not caused by our changes; build uses proper include paths
- Build command: `cmake --build build --config Release` from project root
- GUI: `npm run build` or `npx tsc --noEmit` from `gui/` directory

### Key Files
- `src/cli/config.h` — AppConfig struct, DlssQualityMode enum
- `src/cli/cli_parser.cpp` — CLI argument parsing
- `src/dlss/ngx_wrapper.h/.cpp` — NGX/DLSS SDK wrapper
- `src/dlss/dlss_rr_processor.h/.cpp` — RR processor (to be REPLACED by SR)
- `src/dlss/dlss_fg_processor.h/.cpp` — FG processor (DO NOT TOUCH)
- `src/pipeline/sequence_processor.cpp/.h` — 1954-line pipeline file
- `gui/src/types/dlss-config.ts` — TypeScript DLSS config types
- `gui/src/state/config-store.ts` — Redux-like state management
- `gui/src/components/BasicSettings.tsx` — GUI settings panel
- `gui/electron/cli-args.ts` — CLI args builder
- `blender/dlss_compositor_aov/__init__.py` — Blender addon

### NGX SR API Key Facts
- SR uses `NVSDK_NGX_Feature_SuperSampling` (not RR's `NVSDK_NGX_Feature_RayReconstruction`)
- SR create params: `NVSDK_NGX_DLSS_Create_Params` (nvsdk_ngx_params.h:37-43)
- SR eval params: `NVSDK_NGX_VK_DLSS_Eval_Params` (nvsdk_ngx_helpers_vk.h:64-100)
- SR availability: `NVSDK_NGX_Parameter_SuperSampling_Available` (defs.h:616)
- Preset: `NVSDK_NGX_DLSS_Hint_Render_Preset` enum — J=10, K=11, L=12, M=13
- Helper: `NGX_VULKAN_CREATE_DLSS_EXT1` for creation, `NGX_VULKAN_EVALUATE_DLSS_EXT` for eval
- GBuffer hints: optional pInAttrib array in eval params

### Execution Waves
- Wave 1 (parallel): T1 (C++ config), T4 (GUI types), T9 (Blender addon) — NO dependencies
- Wave 2 (parallel, after T1 and T4): T2 (ngx_wrapper), T3 (dlss_sr_processor), T6 (GUI preset UI)
- Wave 3 (parallel, after T2+T3 and T9): T5 (sequence_processor), T7 (cleanup), T10 (docs)
- Wave 4 (after all): T8 (build verification)
- Final: F1-F4 (parallel reviews)
## Task 1 - SR preset CLI support
- DLSS-SR preset values map directly to NGX preset hints J=10, K=11, L=12, M=13.
- `parsePreset()` can mirror `parseQuality()` exactly with simple string compares and the same error style.
- The CLI help text should say DLSS-SR for `--test-ngx`, not DLSS-RR.
- Default preset should be `L` in `AppConfig` to match the task requirement.
## 2026-04-15 � Task 9 Blender addon pass cleanup
- Removed Glossy Color (specular albedo) from the Blender AOV config since SR does not consume specular separately.
- Kept Combined, Z, Vector, Normal, Diffuse Color, and Roughness AOV passes intact.
- Verified the addon parses cleanly with Python AST after edits.

## 2026-04-15 � Task 4
- Added `preset` to `DlssConfig` with the same union-style pattern as `quality` and defaulted it to `'L'` in `DEFAULT_CONFIG`.
- Added `SET_PRESET` to the config store action union and reducer with a direct state assignment case.
- TypeScript validation passed with `npx tsc --noEmit` from `gui/`.

## 2026-04-15 — Task 2 ngx_wrapper RR→SR
- `NgxContext::createDlssSR()` must use `NVSDK_NGX_DLSS_Create_Params` plus `NGX_VULKAN_CREATE_DLSS_EXT1`; the helper already writes width/height/perf-quality/create-flag parameters before creating `NVSDK_NGX_Feature_SuperSampling`.
- SR preset hint parameter macros live in `nvsdk_ngx_defs.h` at the end of the file: `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_{DLAA,Quality,Balanced,Performance,UltraPerformance,UltraQuality}`.
- SR availability should only read `NVSDK_NGX_Parameter_SuperSampling_*`; the old RR fallback string and `SuperSamplingDenoising_*` params are unnecessary.
- Build required updating downstream call sites/tests to `isDlssSRAvailable()` / `createDlssSR(..., config.preset, ...)` after renaming the wrapper API.
## 2026-04-15 — Task 3 SR processor refactor
- Replaced the RR eval processor files with `dlss_sr_processor.h/.cpp` and switched evaluation to `NVSDK_NGX_VK_DLSS_Eval_Params` plus `NGX_VULKAN_EVALUATE_DLSS_EXT`.
- SR input drops specular fields entirely; diffuse albedo, normals, and roughness map through `GBufferSurface.pInAttrib[]` only when the incoming image handle is non-null.
- `sequence_processor.cpp` and the GPU processor test must include the new SR header/types or the RR file deletion breaks compilation immediately.
## 2026-04-15 — Task 5 sequence_processor SRFG rename
- `SequenceProcessor` now uses `processDirectorySRFG` consistently in the declaration, dispatcher call site, and combined pipeline definition.
- The combined SR+FG path should use `[SRFG]` log prefixes and SR-specific wording such as `SR command buffer` / `SR output` to avoid stale RR naming in diagnostics.
- Renaming the local `DlssSRProcessor` instance to `srProcessor` is sufficient in the combined path; FG-only code remains untouched and the Release build still passes.
## T6 (RR to SR Refactor)
- Setting an electron-vite project to run in a pure browser context requires mocking window.dlssApi via globalThis if it expects IPC preload APIs to exist.
- Added Preset dropdown (preset-select) which mirrors the quality dropdown's visibility logic relying on state.scaleEnabled.
- Dispatched actions such as SET_PRESET directly utilize the exact string literal types inferred from DlssConfig['preset'].
- No [RRFG] or [RR] references were found in progress-parser.ts, as the logging parsing logic matches standard Processing frame N/M outputs rather than specific preset tags.
- Verified TypeScript compilation successfully via npx tsc --noEmit.

## Documentation Update Findings
- Replaced all instances of DLSS Ray Reconstruction (DLSS-RR) with DLSS Super Resolution (DLSS-SR) in README.md and docs/*.md.
- Documented the new \--preset\ CLI option and its available modes (J, K, L, M).
- Updated input specification to reflect that SR uses G-buffer hints (Albedo, Normals, Roughness) instead of RR's specific specular requirements.
- Verified removal of \specularAlbedo\ and \GlossCol\ from input documentation.
- Updated troubleshooting and requirement sections to reflect SR transition.

## 2026-04-15 — Task 7 residual cleanup sweep
- Residual RR references were limited to comments, test strings, and NGX feature discovery code paths.
- `RayReconstruction` needed to become `SuperResolution` in the Vulkan NGX feature discovery path; string-only RR mentions in tests/comments were updated to SR wording.
- The safe search pattern for final verification is to scan `src/` and `gui/src/` separately, then treat `tests/` as a cleanup sweep with expected SDK DLL-name false positives.
