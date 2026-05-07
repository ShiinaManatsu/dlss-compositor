# Phase 4 GUI Polish - Learnings

## 2026-05-07 Initial Codebase Analysis

### Project Structure
- **C++ CLI**: `src/cli/cli_parser.cpp`, `src/cli/config.h`, `src/main.cpp`
- **GUI Electron**: `gui/electron/` (main.ts, preload.ts, cli-args.ts, progress-parser.ts, ipc-channels.ts, settings-store.ts)
- **GUI React**: `gui/src/components/`, `gui/src/state/`, `gui/src/types/`
- **Tests**: `gui/tests/` (Playwright e2e), `tests/` (C++ Catch2), `tests/python/`

### Existing CLI Flags (Already Implemented in C++)
The following flags are ALREADY in `src/cli/cli_parser.cpp` and `src/cli/config.h`:
- `--source-fps` → `config.sourceFps` (default 30) [line 341-353]
- `--sharpness` → `config.sharpness` (float [0.0, 1.0]) [line 383-395]
- `--ngx-log-level` → `config.ngxLogLevel` [line 440-450]
- `--tonemap custom` → already parsed in `parseTonemapMode` [line 117-131]
- `--progress-format` → NOT YET added to CLI

### CLI Parser Does NOT YET Have
- `--progress-format <human|json>` flag → Task 2
- Camera.json auto-discovery → Task 1

### GUI State
- `gui/src/types/dlss-config.ts` - DlssConfig interface MISSING: `sourceFps`, `sharpness`, `ngxLogLevel`
- `gui/src/state/config-store.ts` - config reducer, 28 action types, Context API
- `gui/electron/cli-args.ts` - buildCliArgs function, 68 lines
- `gui/electron/ipc-channels.ts` - 13 IPC channels
- `gui/electron/preload.ts` - contextBridge.exposeInMainWorld

### Key Files Modified Per Task
- Task 1 (CLI camera auto-discover): `src/cli/cli_parser.cpp` or `src/main.cpp`
- Task 2 (NDJSON): New `src/core/ndjson_emitter.h/cpp`, `src/cli/config.h`, `src/cli/cli_parser.cpp`, `src/pipeline/sequence_processor.cpp`
- Task 3 (GUI controls): `gui/src/types/dlss-config.ts`, `gui/src/state/config-store.ts`, `gui/src/components/BasicSettings.tsx`, `gui/src/components/AdvancedSettings.tsx`, `gui/electron/cli-args.ts`, `gui/electron/settings-store.ts`
- Task 4 (tonemap custom): `gui/src/types/dlss-config.ts`, `gui/src/components/AdvancedSettings.tsx`, `gui/electron/cli-args.ts`
- Task 5 (NDJSON parser): `gui/electron/progress-parser.ts`, `gui/electron/main.ts`, `gui/electron/ipc-channels.ts`, `gui/electron/preload.ts`, `gui/src/hooks/useProcessing.ts`
- Task 6 (StatusBar): New `gui/src/components/StatusBar.tsx`, `gui/electron/main.ts`, `gui/src/App.tsx`
- Task 7 (camera auto-detect): New `gui/electron/camera-detector.ts`, `gui/src/components/InputOutputPanel.tsx`, `gui/electron/main.ts`
- Task 8 (ResolutionPanel): New `gui/src/components/ResolutionPanel.tsx`, `gui/src/components/BasicSettings.tsx`, `gui/src/state/config-store.ts`
- Task 9 (Live preview): New `gui/electron/preview-generator.ts`, `gui/src/components/ProcessingView.tsx`, `gui/electron/main.ts`
- Task 10 (ETA + UX): `gui/src/hooks/useProcessing.ts`, `gui/src/components/ProcessingView.tsx`, `gui/src/components/AdvancedSettings.tsx`

### Codebase Patterns
- React Context + useReducer (no Redux/Zustand)
- Tailwind CSS for styling
- Electron IPC: ipcMain.handle (for queries) + ipcMain.on (for fire-and-forget), preload exposes via contextBridge
- Action pattern: `{ type: 'SET_XXX', payload: T }`
- TypeScript strict types, existing components use data-testid for Playwright tests
- advancedSettings uses `const [open, setOpen] = useState(false)` - collapsed by default already

### Important: tonemapMode Type
Current type: `'none' | 'pq'` → needs to add `'custom'`
Current select has only `pq` and `none` options → add `custom`
LUT picker currently enabled when `tonemapMode !== 'none'` → change to `tonemapMode === 'custom'`

### Progress Parser
Current: regex `/Processing frame(?: pair)? (\d+)\/(\d+)/` (FRAGILE)
Needed: JSON.parse(line) for NDJSON mode

### IPC Channels Pattern
- `ipc-channels.ts` (electron side) exports `IPC_CHANNELS` object
- `gui/src/ipc/channels.ts` (renderer side) mirrors the same
- Both must be updated in sync

### CLI Parser Note
`parseNgxLogLevel` in cli_parser.cpp maps 'warn', 'error', 'info' all to `NVSDK_NGX_LOGGING_LEVEL_ON` - this is expected behavior (the NGX SDK only has ON/OFF/VERBOSE, so these are approximate mappings).

## 2026-05-07 Task 1: Camera auto-discovery

### Implementation
- Added `#include <filesystem>` to `src/cli/cli_parser.cpp`
- Inserted auto-discovery block between arg-parsing loop end (line 455) and interpolate validation (line 457)
- Logic: if `cameraDataFile.empty()` AND `!inputDir.empty()`, check `<inputDir>/camera.json`; if exists, set `cameraDataFile` and print info log to stderr
- Runs BEFORE the `--interpolate requires --camera-data` validation, so auto-discovered camera.json satisfies that check

### Test Cases Added to `tests/test_cli.cpp`
1. `auto-discover camera.json from input dir` — creates temp dir with camera.json, runs with `--input-dir <dir> --interpolate 2x`, verifies cameraDataFile is set and parse succeeds
2. `no auto-discover when camera.json missing` — creates temp dir without camera.json, runs with `--input-dir <dir>`, verifies cameraDataFile stays empty
3. `explicit --camera-data wins over auto-discover` — creates temp dir with camera.json but passes explicit `--camera-data other.json`, verifies explicit value wins

### Test Infrastructure
- Added `createTempDir()` helper using `std::filesystem::temp_directory_path()` + counter for uniqueness
- Added `touchFile()` helper using `std::fopen`/`std::fprintf` for creating test files
- All tests use `std::filesystem::remove_all()` for cleanup

### Build/Test Verification
- MSBuild Release: 0 warnings, 0 errors
- All 35 CLI test cases (94 assertions) passed

## 2026-05-07 Task 2: NDJSON progress events (--progress-format json)

### Files Touched
- NEW: src/core/ndjson_emitter.h/.cpp - thread-safe stdout emitter (mutex-guarded fwrite + newline + fflush)
- NEW: 	ests/test_ndjson_emitter.cpp - 5 Catch2 cases (default-disabled, toggle, no-op-when-disabled, thread-safety, singleton identity)
- src/cli/config.h - added enum class ProgressFormat { Human, Json } and progressFormat field on AppConfig
- src/cli/cli_parser.cpp - parses --progress-format <human|json>, updates printHelp()
- src/main.cpp - on Json mode: calls Log::setQuietStdout(true) then enables global emitter (order matters — quiet first so subsequent NGX init logs go to stderr)
- src/core/logger.h/.cpp - added Log::setQuietStdout(bool); Log::info now routes console mirror to stderr instead of stdout when quiet flag set (log file mirror unchanged)
- src/pipeline/sequence_processor.cpp - emits started/frame_started/frame_written/error/complete in processDirectory SR-only path; uses 
lohmann::json for safe escaping (paths contain backslashes on Windows)
- CMakeLists.txt - added src/core/ndjson_emitter.cpp to dlss_compositor_core sources
- 	ests/CMakeLists.txt - added 	est_ndjson_emitter.cpp

### Design Decisions
- Used Log::setQuietStdout(bool) toggle in logger rather than make Log::info depend on NdjsonEmitter — avoids circular dep within core/ and is a cleaner separation (the toggle is general-purpose).
- Used 
lohmann::json::dump() for event construction instead of hand-rolled string building — backslash-escaping in Windows paths is otherwise easy to get wrong.
- Made globalNdjsonEmitter() a Meyers singleton (function-local static) for trivial init order.
- Single mutex around enabled-flag AND fwrite/newline/fflush, so multi-line atomicity is guaranteed even under thread contention.
- Added the events only in the SR-only processDirectory (not yet in processDirectorySRFG / processDirectoryFG) per task scope ("main processing loop") — those are wired to the same processDirectory() entrypoint but currently dispatch separately. Future work could extend if SRFG/FG paths need progress reporting via NDJSON.

### Pitfall Hit
- Initially the lambda in test_ndjson_emitter.cpp captured kIterations implicitly; MSVC C3493 with [&emitter, t]. Fixed by adding kIterations to the capture list. (GCC/Clang are more lenient with constexpr globals.)
- NGX wrapper writes via Log::info() which used to hit stdout unconditionally. Adding the Log::setQuietStdout knob fixed the leak without modifying every NGX/Vulkan log call site.

### Verification (from .sisyphus/evidence/phase4/task-2-ndjson.txt)
- cmake --build build --config Release → success
- ctest -R Ndjson → 5/5 passed
- Default --progress-format human (or no flag) → stdout output unchanged (verified against same input/output)
- --progress-format json on 5-frame fixture → 12 NDJSON lines on stdout (started + 5*(frame_started+frame_written) + complete), all NGX/human logs on stderr
- Pre-existing test failures (frame_prefetcher_*, dlss_fg_e2e_interpolation) confirmed unrelated to this change by running ctest on stashed working tree (same 4 failures both before and after)

## 2026-05-07 Task 3: GUI Controls for sourceFps, sharpness, ngxLogLevel

### Implementation
- Added fields to DlssConfig and DEFAULT_CONFIG.
- Updated config-store reducer with 3 new actions.
- Added \
Source
FPS\ numeric input to BasicSettings below Upscaling.
- Added \Sharpness\ slider and \NGX
Log
Level\ select to AdvancedSettings.
- Updated cli-args to conditionally emit --source-fps, --sharpness, --ngx-log-level.
- Updated settings-store schema, defaults, and save/load logic.

### Build/Verification
- cd gui && npm run build passed with 0 errors.
- Git committed.

## 2026-05-07 Task 4: Custom tonemap mode

### Implementation
- Extended tonemapMode type to 'none' | 'pq' | 'custom' in dlss-config.ts
- Added 'custom (LUT files)' option to tonemap select in AdvancedSettings.tsx
- LUT pickers now enable only when tonemapMode === 'custom' (was: tonemapMode !== 'none')
- Updated no-inverse-tonemap checkbox to enable only when tonemapMode === 'custom'
- Added validation hint text below forward LUT input when custom+LUT missing
- cli-args.ts: custom mode pushes '--tonemap custom' flag
- ProcessingView.tsx: Start button disabled when custom+no forward LUT + shows warning

### Build/Verification
- cd gui && npm run build passed with 0 errors.
- Git committed.

## Task 5 — NDJSON parser
- Renderer-side hook now owns latestFramePath; frame:latest IPC kept as fallback only.
- ProgressEvent discriminated union ensures TypeScript narrows on switch(event.event).
- Non-JSON stdout still routes to PROCESS_LOG so legacy binaries (no --progress-format) keep showing logs (progress just won't update).
- start() in useProcessing must reset latestFramePath to null so the previous run's path doesn't leak into the new run.

 -   A d d e d   a u t o - d i s c o v e r y   f o r   c a m e r a . j s o n   i n   G U I   v i a   I P C   h a n d l e   ' c a m e r a : a u t o - d e t e c t '   w h i c h   r e t u r n s   b o t h   p a t h   a n d   r e s o l u t i o n .  
 