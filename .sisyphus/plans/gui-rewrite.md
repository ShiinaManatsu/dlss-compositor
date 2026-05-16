# GUI Rewrite — Standard EXR, Aspect-Locked Resolution, SR/FG Separation, Bug Fixes, Dev Experience

## TL;DR

> **Quick Summary**: A comprehensive overhaul of the Electron GUI plus its data sources: switch Blender exporter from MultiLayer EXR to standard EXR (so RenderDoc can open files), read source resolution from the EXR header (not `camera.json`), enforce aspect-locked resolution UX (single W or H input + bidirectional scale), separate SR and FG settings into distinct panels, expose every NGX advanced setting added by Plan 1, delete the executable-path picker (auto-detect dev vs packaged), persist settings to `gui/dlss-compositor.json`, fix the DLSS-detection hang and FFmpeg preview failures, and add VSCode F5 dev launch configurations.
>
> **Deliverables**:
> - Blender exporter writes standard EXR with panel UI for compression + render scale
> - C++ EXR reader compatible with both MultiLayer and standard channel naming
> - New CLI flag `--inspect-exr <file>` for header-only metadata extraction
> - GUI reads EXR header via IPC (no more `camera.json` dependency for resolution)
> - `ResolutionPanel` rewrite: aspect-locked, orientation-aware presets, bidirectional scale
> - `SRSettings.tsx` + `FGSettings.tsx` split from monolithic `BasicSettings.tsx`
> - Advanced settings panel exposing all Plan 1 NGX flags
> - `ExePathConfig.tsx` deleted; auto-detect with dev/packaged fallback
> - `gui/dlss-compositor.json` settings persistence (replaces Electron userData)
> - `.vscode/launch.json` with 3 debug configurations
> - DLSS detection diagnosed and fixed (root-caused via real CLI invocation, not band-aid timeout)
> - FFmpeg preview with structured error reporting and graceful degradation
> - `ProcessingView.tsx` simplified to preview-only (no duplicate progress)
> - Skip-and-warn for malformed EXR filenames (e.g. `filename_.exr`)
> - 5 new Playwright specs covering all new flows
> - README + 3 new docs (`gui_guide.md` rewrite, `blender_export_guide.md`, `development.md`)
>
> **Estimated Effort**: XL
> **Parallel Execution**: YES — 5 waves
> **Critical Path**: T1 (Blender format) → T3 (EXR header IPC) → T6 (Resolution UX) → T7 (SR/FG split) → T15 (Playwright) → F1-F4

---

## Context

### Original Request
After atlas implemented the Phase 0–4 plans on the `opus-rework` branch, real-world use surfaced 14 distinct issues spanning data sources (Blender format, source-resolution detection), UX (resolution panel UX, SR/FG conflated, redundant progress), reliability (DLSS detect hang, FFmpeg silent failures, malformed-file handling), architecture (CLI path picker is friction, settings persistence in wrong place, no F5 dev workflow), and missing features (no advanced NGX flag exposure). User wants a single sweeping rework rather than 14 micro-fixes.

### Interview Summary

**Key Discussions** (all captured in `.sisyphus/handoff/2026-rework-decisions.md`):

- **Standard EXR over MultiLayer**: Pain point is that current MultiLayer EXR cannot be opened in RenderDoc (Adobe Premiere can, but that defeats inspection). Standard EXR with R/G/B/A/Z channel naming opens in any tool. Constraint: motion vectors + depth must still survive — channel naming workarounds needed.
- **Settings file**: `gui/dlss-compositor.json` (project-relative, not Electron userData). User wants version-controllable defaults, separate from "user data hidden in AppData".
- **No CLI path picker**: GUI ships with CLI binary; auto-detect handles both dev and packaged modes. Removes a class of "wrong path" bugs entirely.
- **All NGX advanced settings exposed**: Power-user mode. Defaults are NVIDIA recommendation (Auto), individual overrides available. Surfaces in dedicated "Advanced" sub-panel within SR settings.
- **Per-quality preset config in GUI**: User picks a preset for each quality mode independently (mirrors Plan 1's CLI flags). UI: 6 dropdowns under SR > Advanced > Presets.
- **Aspect ratio LOCKED, no toggle**: User cannot change picture aspect. Input W or H only; the other is computed (floor). Orientation auto-detected (landscape vs portrait), presets adjust accordingly. Bidirectional: edit W/H → scale updates; edit scale → W/H updates.
- **DLSS detect hang is NOT a timeout problem**: Likely stdout/stderr routing mismatch or NDJSON schema mismatch between CLI and GUI. Fix via real diagnostic (run CLI manually, compare to GUI parse code), not by adding a timeout band-aid.
- **SR vs FG separation**: Distinct panels with explicit labels. SR settings: scale, quality, presets, sharpness, source-fps, ngx-log-level. FG settings: interpolation factor, camera-data file, tonemap mode, LUT files. Common: input/output, memory budget, output passes, video encode.
- **FFmpeg preview**: Structured error states (`ffmpeg-not-found`, `exr-decode-error`, `timeout`, `unknown`) with friendly UI degradation, not silent null.
- **Right column simplification**: User explicitly said "右边栏上面已经有进度了, 其实只显示预览就行了". Remove duplicate progress bar, make preview the focus.
- **Malformed EXR**: Skip files without trailing frame number (e.g. `filename_.exr`) with warning log. Adobe-PR style "pick first frame" deferred to future work.

**Research Findings** (atlas opus-rework audit):
- Atlas's `gui/electron/main.ts:20-33` banner probe has no timeout AND no error fallback (root cause of hang)
- Atlas's `gui/electron/camera-detector.ts:18-30` reads `render_width/render_height` from `camera.json` — wrong source (Blender-side render config, not actual EXR output)
- Atlas's `gui/electron/preview-generator.ts:9-50` swallows FFmpeg errors silently
- Atlas's `gui/src/components/ResolutionPanel.tsx:59-89` allows independent W/H (e.g. user can set 1920×800 with 2:1 source — broken aspect)
- Atlas's `gui/src/components/BasicSettings.tsx:34-106` mixes SR + FG settings
- Master C++ at `src/pipeline/sequence_processor.cpp:677-694` already reads resolution from EXR header — atlas's GUI is the regression, C++ is correct
- Master `src/pipeline/sequence_processor.cpp:85` (`extractTrailingFrameNumber`) returns false for missing frame number but processing loop sets `frameNumber = INT_MAX` and continues — needs to skip-and-warn instead

### Metis Review

Same provenance as Plan 1: `.sisyphus/handoff/2026-rework-decisions.md` already encodes the equivalent of Metis-style gap analysis. Key gaps already addressed:

- **Gap**: Standard EXR doesn't natively support arbitrary channel names like `Vector.X/Y/Z/W`. → **Resolved**: Use a documented channel mapping convention (e.g., suffix-based: `MV.X`, `MV.Y`) and update C++ alias map in `src/core/exr_reader.cpp:33-63` to recognize both old and new naming.
- **Gap**: When source EXR header read fails, what does ResolutionPanel show? → **Resolved**: Warning banner + manual scale input fallback; presets/single-input disabled until valid source detected.
- **Gap**: How does `inspect-exr` IPC handle long paths/non-ASCII? → **Resolved**: GUI passes UTF-8 path via stdin or argv with proper encoding; CLI uses `wmain` or argument byte-string conversion.
- **Gap**: Settings file conflict between dev environments (multiple checkouts)? → **Resolved**: User accepted that gui/dlss-compositor.json may differ per checkout; .gitignore the file (it's user prefs); commit a `dlss-compositor.example.json` template instead.
- **Gap**: F5 launch behavior when build/Release/dlss-compositor.exe doesn't exist? → **Resolved**: Add pre-launch task in launch.json that runs `cmake --build build --config Release` first.

---

## Work Objectives

### Core Objective
Deliver a GUI that "just works": user installs the package, opens the GUI, points at a Blender EXR sequence, sees source resolution detected from the actual file, adjusts target resolution via a single intuitive control with locked aspect, sees clearly separated SR and FG settings, gets immediate visual feedback on preview, and never sees a silent hang or null state.

### Concrete Deliverables
1. `blender/dlss_compositor_aov/__init__.py` + `blender/aov_export_preset.py`: switch to `OPEN_EXR` (non-MultiLayer), update channel naming, add panel UI for compression dropdown + render scale slider
2. `src/core/exr_reader.cpp`: extended channel alias map covering both MultiLayer and standard formats
3. `src/cli/cli_parser.cpp` + new `src/inspection/exr_inspector.cpp`: `--inspect-exr <file>` flag emitting JSON `{width,height,channels[]}`
4. `gui/electron/preload.ts` + `gui/electron/main.ts`: new `exr:read-header` IPC channel
5. `gui/electron/camera-detector.ts`: removal of `readSourceResolution` (or refactor to other camera fields only)
6. `src/pipeline/sequence_processor.cpp`: skip-and-warn for files without trailing frame number
7. `gui/src/components/ResolutionPanel.tsx`: complete rewrite (aspect-locked, orientation-aware presets, bidirectional scale)
8. `gui/src/components/SRSettings.tsx` (new), `gui/src/components/FGSettings.tsx` (new), `gui/src/components/CommonSettings.tsx` (new); deletion or thin-wrappering of `BasicSettings.tsx`
9. Advanced sub-panel inside `SRSettings.tsx` exposing all Plan 1 NGX flags
10. Deletion of `gui/src/components/ExePathConfig.tsx`; auto-detection in `gui/electron/main.ts`
11. `gui/electron/settings-store.ts` rewrite to persist to `gui/dlss-compositor.json`
12. `.vscode/launch.json` with 3 configurations
13. `gui/electron/main.ts:20-33`: DLSS detection diagnosed + fixed (root cause + fix, plus sanity error state)
14. `gui/electron/preview-generator.ts`: structured error returns + GUI degradation
15. `gui/src/components/ProcessingView.tsx`: simplified to preview-only
16. 5 new Playwright specs (`gui/tests/exr-source-resolution.spec.ts`, `gui/tests/resolution-aspect-lock.spec.ts`, `gui/tests/sr-fg-separation.spec.ts`, `gui/tests/dlss-detect.spec.ts`, `gui/tests/preview-error.spec.ts`) + update to `gui/tests/real-processing.spec.ts`
17. README rewrite of GUI flow + new `docs/blender_export_guide.md` + new `docs/development.md` + `docs/gui_guide.md` rewrite

### Definition of Done
- [ ] `cmake --build build --config Release` succeeds
- [ ] `cd gui ; npm run typecheck` succeeds (zero errors)
- [ ] `cd gui ; npm run lint` succeeds (zero errors)
- [ ] `cd gui ; npx playwright test` — 100% pass on all specs (new + updated)
- [ ] Manual flow: install Blender extension → render 5 frames → open GUI → see correct source resolution from EXR → set target via single H input → start processing → see live preview → completion
- [ ] `dlss-compositor.exe --inspect-exr fixtures/sintel_short/frame_0001.exr` outputs valid JSON
- [ ] No reference to `render_width`/`render_height` from `camera.json` in any GUI source file (`grep -r render_width gui/`)
- [ ] No `ExePathConfig` reference in any GUI source file
- [ ] `gui/dlss-compositor.json` exists after first run; settings persist across restarts
- [ ] F5 in VSCode launches GUI in dev mode, hits a breakpoint in `main.ts`
- [ ] Standard EXR output from Blender opens in RenderDoc

### Must Have
- Aspect ratio is **fully locked** — user cannot construct a 1920×800 frame from a 16:9 source
- Source resolution comes from EXR header, never from `camera.json`
- DLSS detection failure mode is "explicit error in StatusBar", never silent hang
- FFmpeg failure mode is "structured error with user-facing remedy", never silent null
- Settings persist to `gui/dlss-compositor.json` and survive GUI restarts
- All Plan 1 NGX flags accessible via GUI advanced sub-panel
- F5 works for all 3 launch configs without manual setup beyond first build

### Must NOT Have (Guardrails)
- ❌ Do NOT touch core processing pipeline beyond skip-and-warn for malformed filenames (no DLSS algorithm changes — Plan 1 owns that)
- ❌ Do NOT add Electron userData persistence (gui/dlss-compositor.json is the only store)
- ❌ Do NOT add a CLI executable path picker back as "advanced option" — it's deleted, period
- ❌ Do NOT add aspect-ratio toggle as "advanced" — locked is locked
- ❌ Do NOT make standard EXR optional via Blender preference — it's the only mode
- ❌ Do NOT add new third-party dependencies for EXR parsing in JS (use IPC to C++ instead)
- ❌ Do NOT add timeout to DLSS detection as primary fix — diagnose stdio routing first
- ❌ Do NOT preserve atlas's `BasicSettings.tsx` API — break the import contract, force callers to migrate to SR/FG/Common
- ❌ AI-slop: do NOT generate verbose JSDoc for trivial React components
- ❌ AI-slop: do NOT introduce a `<ResolutionContext.Provider>` wrapper just for aspect-lock — local state in `ResolutionPanel` is fine
- ❌ AI-slop: do NOT add "internationalization" hooks (project is English-only)
- ❌ Do NOT delete `camera.json` schema fields used by C++ FG path (`render_width`/`render_height` are still consumed by `src/core/camera_data_loader.cpp`); only the GUI's use is removed
- ❌ Do NOT commit user's actual `gui/dlss-compositor.json` (.gitignore it; commit `dlss-compositor.example.json` instead)
- ❌ Do NOT touch `gui/electron/process-manager.ts` core spawn logic (keep architecture; only modify what's necessary for diagnosis fixes)

---

## Verification Strategy (MANDATORY)

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed.

### Test Decision
- **Infrastructure exists**: YES — Playwright is set up under `gui/tests/`, CTest exists for C++ side
- **Automated tests**: YES (Tests after implementation) — Playwright specs added per task
- **Framework**: Playwright (GUI), CTest+GoogleTest (C++ inspector)
- **Per-task QA**: Each task includes Playwright or `Bash` agent-executed scenarios

### QA Policy
Every task MUST include agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/gui-rewrite/task-{N}-{slug}.{ext}`.

- **Frontend/UI**: Use Playwright MCP — navigate, fill, click, assert DOM, screenshot
- **CLI/Inspector**: Use Bash — run binary, parse JSON, assert fields
- **IPC**: Playwright invokes Electron, asserts IPC round-trip via UI state changes
- **Blender exporter**: Bash launches Blender headless with a test .blend, checks output

---

## Execution Strategy

### Parallel Execution Waves

> Maximize throughput. 16 tasks distributed across 5 waves; Wave 1 has 5 parallel tasks (foundation), Waves 2-3 maximize concurrency.

```
Wave 1 (Start Immediately — independent foundation work, 5 parallel):
├── Task 1:  Blender → standard EXR + panel UI                        [unspecified-high]
├── Task 2:  C++ EXR reader compatibility for both formats            [deep]
├── Task 5:  Skip-and-warn malformed EXR filenames                    [quick]
├── Task 10: Delete ExePathConfig + auto-detect exe path              [unspecified-high]
└── Task 12: VSCode F5 dev launch configurations                      [quick]

Wave 2 (After Wave 1 — IPC/data layer, 3 parallel):
├── Task 3:  --inspect-exr CLI flag + IPC channel                     [unspecified-high]
├── Task 11: settings-store.ts → gui/dlss-compositor.json             [unspecified-high]
└── Task 13: DLSS detect diagnosis + fix                              [deep]

Wave 3 (After Wave 2 — UI components consuming new IPC, 4 parallel):
├── Task 4:  Remove camera.json resolution dep in GUI                 [quick]
├── Task 6:  ResolutionPanel rewrite (aspect-locked + bidirectional)  [visual-engineering]
├── Task 14: FFmpeg preview structured errors + GUI degradation       [unspecified-high]
└── Task 15: ProcessingView simplification (preview-only)             [visual-engineering]

Wave 4 (After Wave 3 — settings UI restructure, 2 parallel):
├── Task 7:  SR / FG / Common panel split                             [visual-engineering]
└── Task 8:  Advanced sub-panel — expose Plan 1 NGX flags             [unspecified-high]

Wave 5 (After Wave 4 — verification + docs, 2 parallel):
├── Task 9:  Playwright spec suite (5 new + 1 updated)                [unspecified-high]
└── Task 16: README + 3 docs rewrite                                  [writing]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit                              [oracle]
├── Task F2: Code quality review                                [unspecified-high]
├── Task F3: Real manual QA on built GUI + Blender              [unspecified-high]
└── Task F4: Scope fidelity check                               [deep]
-> Present results -> Get explicit user okay

Critical Path: T1 → T3 → T4 → T6 → T7 → T9 → F1-F4
Parallel Speedup: ~65% faster than sequential
Max Concurrent: 5 (Wave 1)
```

### Dependency Matrix

- **T1**: blocks → T2 (must know new channel names), T9 | blocked by → none
- **T2**: blocks → T3, T9                              | blocked by → T1
- **T3**: blocks → T4, T6                              | blocked by → T2
- **T4**: blocks → T6                                  | blocked by → T3
- **T5**: blocks → T9                                  | blocked by → none
- **T6**: blocks → T7, T9                              | blocked by → T3, T4
- **T7**: blocks → T8, T9                              | blocked by → T6
- **T8**: blocks → T9                                  | blocked by → T7 (needs SRSettings to extend)
- **T9**: blocks → F1-F4                               | blocked by → T1, T2, T5, T6, T7, T8, T13, T14
- **T10**: blocks → T11, T13                           | blocked by → none
- **T11**: blocks → T9                                 | blocked by → T10
- **T12**: blocks → none                               | blocked by → none (truly independent)
- **T13**: blocks → T9                                 | blocked by → T10
- **T14**: blocks → T9                                 | blocked by → T11
- **T15**: blocks → T9                                 | blocked by → T6 (depends on layout space freed)
- **T16**: blocks → none (parallel with T9)            | blocked by → T1, T6, T7, T10, T12

### Agent Dispatch Summary

- **Wave 1**: 5 — T1/T10 → `unspecified-high`, T2 → `deep`, T5/T12 → `quick`
- **Wave 2**: 3 — T3/T11 → `unspecified-high`, T13 → `deep`
- **Wave 3**: 4 — T4 → `quick`, T6/T15 → `visual-engineering`, T14 → `unspecified-high`
- **Wave 4**: 2 — T7 → `visual-engineering`, T8 → `unspecified-high`
- **Wave 5**: 2 — T9 → `unspecified-high`, T16 → `writing`
- **FINAL**: 4 — F1 → `oracle`, F2/F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

> Implementation + Test = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.

- [x] 1. **Blender exporter — switch to standard EXR + panel UI for compression and render scale**

  **What to do**:
  - Modify `blender/dlss_compositor_aov/__init__.py`:
    - Change scene render setting `image_settings.file_format` from `'OPEN_EXR_MULTILAYER'` → `'OPEN_EXR'`
    - Channel mapping for standard EXR (which only allows R/G/B/A and grouped channels):
      - Combined → use the standard `R/G/B/A` channels (Cycles default already maps Image to RGBA)
      - Depth → use standard `Z` channel name
      - Motion vectors → write as a separate file pass with naming convention `<frame>.mv.exr` containing R/G channels for forward 2D MV (X→R, Y→G); document this convention in the panel UI tooltip
      - Alternative considered & rejected: arbitrary-named channels in a single EXR (not supported by standard EXR, would force fallback to MultiLayer)
    - Verify Blender Compositor "File Output" node configuration accommodates the split (one file for combined+depth, one for MV) — adjust node tree setup function accordingly
  - Add panel UI in the existing `DLSSCompositorPanel` class (search current panel implementation in `__init__.py`):
    ```python
    layout.prop(scene, "dlss_compositor_compression", text="EXR Compression")
    layout.prop(scene, "dlss_compositor_render_scale", text="Render Scale", slider=True)
    ```
    - Register Scene properties:
      - `dlss_compositor_compression` = EnumProperty with items `[('NONE','None'), ('ZIP','ZIP (lossless)'), ('ZIPS','ZIPS (lossless, scanline)'), ('PIZ','PIZ (lossless)'), ('DWAA','DWAA (lossy)'), ('DWAB','DWAB (lossy)')]`, default `'ZIP'`
      - `dlss_compositor_render_scale` = FloatProperty min=0.1, max=2.0, default=1.0, step=0.05, subtype='FACTOR'
    - On render trigger, write `image_settings.exr_codec = scene.dlss_compositor_compression`
    - Apply render scale: multiply scene `render.resolution_x` and `render.resolution_y` by `dlss_compositor_render_scale`, store original values to restore post-render (use existing render handler pattern)
  - Same modifications to `blender/aov_export_preset.py` if it duplicates the format-setting logic
  - Update `blender/dlss_compositor_aov/__init__.py` `bl_info` version bump (e.g., 0.1.0 → 0.2.0) and changelog entry in module docstring
  - Verification: render 5 frames headless via `blender -b test.blend -a` → confirm output filenames + that they can be opened with `oiiotool --info` showing standard EXR format

  **Must NOT do**:
  - Do NOT preserve a "use MultiLayer" toggle for "backward compatibility"
  - Do NOT include Cryptomatte or other non-essential passes (keep to Combined, Depth, MV)
  - Do NOT touch C++ EXR reader (Task 2 handles that)
  - Do NOT change panel layout beyond adding 2 controls
  - Do NOT add file format other than EXR as alternative

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Blender Python API requires careful attention to property registration / unregistration ordering; cross-cuts both file format and UI panel; needs verification via headless render
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T2, T5, T10, T12)
  - **Blocks**: T2 (C++ reader needs to know channel naming), T9 (Playwright fixtures need standard EXRs)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `blender/dlss_compositor_aov/__init__.py:147-150` — current `render_width/render_height` write to camera.json (LEAVE intact, still consumed by FG path)
  - `blender/aov_export_preset.py:151-154` — same; check if format-setting logic is duplicated
  - Existing panel `DLSSCompositorPanel` class — pattern for `layout.prop(scene, ...)`

  **API/Type References**:
  - Blender Python API: `bpy.types.Scene`, `bpy.props.EnumProperty`, `bpy.props.FloatProperty`
  - Blender Image settings: `scene.render.image_settings.file_format`, `image_settings.exr_codec`
  - OpenEXR codec list: NONE, ZIP, ZIPS, PIZ, PXR24, B44, B44A, DWAA, DWAB

  **External References**:
  - Blender docs: `https://docs.blender.org/api/current/bpy.types.ImageFormatSettings.html`
  - OpenEXR spec on standard channel naming: `https://openexr.com/en/latest/TechnicalIntroduction.html#standard-channel-names`

  **WHY Each Reference Matters**:
  - Standard channel names doc — explains why `Vector.X/Y` doesn't work in standard EXR and motivates the split-file approach
  - Blender ImageFormatSettings doc — confirms `exr_codec` enum values

  **Acceptance Criteria**:

  - [ ] Headless Blender render produces standard EXR (verified via `oiiotool --info` output)
  - [ ] Output file opens in RenderDoc (manual verification noted in evidence)
  - [ ] Panel shows compression dropdown + render scale slider after extension reload
  - [ ] Render scale 50% produces output with half-resolution dimensions

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Headless render produces standard EXR with chosen compression
    Tool: Bash (PowerShell)
    Preconditions: Blender 4.2+ in PATH; test scene fixtures/blender/test_scene.blend with extension installed; oiiotool from OpenImageIO in PATH (or use a small Python script with OpenEXR Python bindings)
    Steps:
      1. Run: blender -b fixtures/blender/test_scene.blend -P fixtures/blender/scripts/set_compression_zip.py -a -s 1 -e 5 -o tmp/render/####
      2. For each output frame: oiiotool --info tmp/render/0001.exr
      3. Assert output contains "openexr" format AND "compression: zip"
      4. Assert NOT "subimages: " > 1 (MultiLayer would have multiple subimages)
    Expected Result: 5 standard EXR files with ZIP compression
    Failure Indicators: Format reported as "openexr-multipart" or compression mismatched
    Evidence: .sisyphus/evidence/gui-rewrite/task-1-headless-render.txt

  Scenario: Render scale slider halves output resolution
    Tool: Bash (PowerShell)
    Preconditions: Same setup; script that sets dlss_compositor_render_scale=0.5 before render
    Steps:
      1. Render base resolution (e.g., scene.render.resolution_x=1920, y=1080) with scale=0.5
      2. oiiotool --info tmp/render/0001.exr
      3. Assert width=960, height=540
    Expected Result: Output dimensions are 50% of base
    Evidence: .sisyphus/evidence/gui-rewrite/task-1-render-scale.txt

  Scenario: Panel UI controls present in Blender
    Tool: Bash (PowerShell) — Blender Python introspection
    Preconditions: Extension installed
    Steps:
      1. Run: blender -b -P scripts/check_panel.py (script that imports scene, checks hasattr(scene, 'dlss_compositor_compression') and 'dlss_compositor_render_scale')
      2. Assert both attributes exist with expected types
    Expected Result: Both Scene props registered correctly
    Evidence: .sisyphus/evidence/gui-rewrite/task-1-panel-props.txt

  Scenario: Compression options all valid
    Tool: Bash (PowerShell)
    Preconditions: Same
    Steps:
      1. For each codec in [ZIP, ZIPS, PIZ, DWAA, DWAB, NONE]: run a 1-frame render with that codec set, verify oiiotool reports matching compression
    Expected Result: All 6 codecs produce valid EXR output
    Evidence: .sisyphus/evidence/gui-rewrite/task-1-codecs.txt
  ```

  **Evidence to Capture**:
  - [ ] task-1-headless-render.txt
  - [ ] task-1-render-scale.txt
  - [ ] task-1-panel-props.txt
  - [ ] task-1-codecs.txt
  - [ ] task-1-renderdoc-screenshot.png (manual capture of standard EXR opened in RenderDoc — proves the original user pain point is resolved)

  **Commit**: YES (1/16)
  - Files: `blender/dlss_compositor_aov/__init__.py`, `blender/aov_export_preset.py`, `fixtures/blender/test_scene.blend`, `fixtures/blender/scripts/set_compression_zip.py`, `fixtures/blender/scripts/check_panel.py`
  - Pre-commit: headless render smoke test

- [x] 2. **C++ EXR reader — support both MultiLayer and standard channel naming**

  **What to do**:
  - Open `src/core/exr_reader.cpp:33-63` — extend the existing channel alias map. Current map probably has entries like `{"DiffCol", "RenderLayer.DiffCol"}` etc. Add standard-EXR aliases:
    ```cpp
    // Standard EXR aliases (from Plan 2 Task 1 Blender output)
    {"R", "Combined.R"},  // or whatever the canonical internal name is
    {"G", "Combined.G"},
    {"B", "Combined.B"},
    {"A", "Combined.A"},
    {"Z", "Depth.Z"},
    // MV files separate; reader signature should accept optional MV path
    ```
  - Refactor reader signature if needed: instead of "one EXR file containing all passes", support "primary EXR + optional auxiliary MV EXR file". Preserve MultiLayer code path completely (master fixtures still use it). Detect format by checking if file is multi-part (use OpenEXR `MultiPartInputFile::parts() > 1`); route to MultiLayer path if so, standard path otherwise.
  - Update reader API in `src/core/exr_reader.h`:
    ```cpp
    struct ExrSequenceFiles {
        std::filesystem::path mainPath;
        std::optional<std::filesystem::path> mvPath;  // empty for MultiLayer
    };
    bool readFrame(const ExrSequenceFiles& files, FrameData& out, std::string& errorMsg);
    ```
  - Update callers in `src/pipeline/sequence_processor.cpp` to construct `ExrSequenceFiles` (auto-pair `<frame>.exr` with `<frame>.mv.exr` if both exist, fall back to single-file MultiLayer if no `.mv.exr` sibling)
  - Add unit test `tests/test_exr_channel_aliases.cpp`:
    - Use a small standard EXR fixture with R/G/B/A/Z + sibling .mv.exr (commit as `tests/fixtures/standard_exr/frame_0001.exr` + `frame_0001.mv.exr`)
    - Use existing MultiLayer fixture
    - Both should produce equivalent `FrameData` (same dims, channel content within tolerance)
    - Edge case: standard EXR without sibling MV → reader fills MV with zeros, returns flag indicating MV missing (caller decides what to do — likely emit warning, continue with empty MV which DLSS-SR handles via reset/jitter alone)

  **Must NOT do**:
  - Do NOT remove MultiLayer support — both formats coexist
  - Do NOT silently use zero MV when MV file is missing on standard format — emit explicit warning via callback or log
  - Do NOT change `FrameData` struct shape (downstream consumers depend on it)
  - Do NOT add a runtime config flag to "force MultiLayer" — auto-detection only
  - Do NOT touch FG path

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Critical data path; format detection logic must be bulletproof; alias map changes can silently break parsing
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T1, T5, T10, T12)
  - **Parallel Group**: Wave 1
  - **Blocks**: T3 (inspector reuses reader's format detection)
  - **Blocked By**: T1 (must know finalized standard channel naming)

  **References**:

  **Pattern References**:
  - `src/core/exr_reader.cpp:33-63` — channel alias map (the focal point)
  - `src/pipeline/sequence_processor.cpp:677-694` — already reads resolution from EXR header (preserve this behavior)
  - `src/pipeline/sequence_processor.cpp:583` — call site of `extractTrailingFrameNumber` — companion area for sibling-file pairing logic

  **API/Type References**:
  - OpenEXR: `Imf::MultiPartInputFile`, `Imf::InputFile`, `Imf::Header`, `Imf::ChannelList`
  - Project type: `FrameData` struct in `src/core/frame_data.h`

  **External References**:
  - OpenEXR docs: `https://openexr.com/en/latest/`
  - OpenEXR multi-part files spec for detection logic

  **WHY Each Reference Matters**:
  - `sequence_processor.cpp:677-694` — proves the C++ side already does the right thing for resolution reading; the reader just needs to extend channel handling
  - `MultiPartInputFile::parts()` — the canonical detection mechanism; using anything else (like file extension, header field heuristics) would be fragile

  **Acceptance Criteria**:

  - [ ] `tests/test_exr_channel_aliases.cpp` covers MultiLayer + standard + missing-MV cases
  - [ ] `ctest -C Release -R ExrChannelAliases` → all PASS
  - [ ] No regression in `ctest -C Release` overall

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Standard EXR + sibling MV file produces same FrameData as MultiLayer
    Tool: Unit test (Bash)
    Preconditions: Both fixture variants prepared
    Steps:
      1. Run: ctest -C Release -R ExrChannelAliases --output-on-failure
      2. Inspect test output — assert width/height/channel-count match between formats
    Expected Result: All test cases PASS
    Evidence: .sisyphus/evidence/gui-rewrite/task-2-unit-tests.txt

  Scenario: Pipeline processes standard EXR end-to-end
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; standard EXR fixture available at fixtures/standard_exr/
    Steps:
      1. Run: dlss-compositor.exe --input-dir fixtures/standard_exr --output-dir tmp/std-out --scale 2 --quality Quality
      2. Verify exit code 0
      3. Verify output EXR files exist and are valid (oiiotool --info)
    Expected Result: Successful processing of standard EXR sequence
    Evidence: .sisyphus/evidence/gui-rewrite/task-2-pipeline-standard.txt

  Scenario: Standard EXR without sibling MV emits warning, continues
    Tool: Bash (PowerShell)
    Preconditions: Standard EXR fixture without .mv.exr siblings
    Steps:
      1. Run: dlss-compositor.exe --input-dir fixtures/standard_exr_no_mv --output-dir tmp/no-mv --scale 2 --progress-format json 2> tmp/no-mv.ndjson
      2. Select-String -Path tmp/no-mv.ndjson -Pattern '"warning":"missing_mv_file"'
    Expected Result: At least one warning event; processing completes (not error)
    Failure Indicators: Process errors out OR no warning event
    Evidence: .sisyphus/evidence/gui-rewrite/task-2-no-mv-warning.ndjson

  Scenario: MultiLayer fixtures still work (regression check)
    Tool: Bash (PowerShell)
    Preconditions: Existing MultiLayer fixtures intact
    Steps:
      1. Run: dlss-compositor.exe --input-dir fixtures/sintel_short --output-dir tmp/multilayer --scale 2 --quality Quality
      2. Verify exit code 0; output binary identical to pre-plan baseline (or within NGX-determinism tolerance)
    Expected Result: No regression
    Evidence: .sisyphus/evidence/gui-rewrite/task-2-multilayer-regression.txt
  ```

  **Evidence to Capture**:
  - [ ] task-2-unit-tests.txt
  - [ ] task-2-pipeline-standard.txt
  - [ ] task-2-no-mv-warning.ndjson
  - [ ] task-2-multilayer-regression.txt

  **Commit**: YES (2/16)
  - Files: `src/core/exr_reader.cpp`, `src/core/exr_reader.h`, `src/pipeline/sequence_processor.cpp`, `tests/test_exr_channel_aliases.cpp`, `tests/CMakeLists.txt`, `tests/fixtures/standard_exr/` (small synthetic EXRs)
  - Pre-commit: `ctest -C Release -R ExrChannelAliases`

- [x] 5. **Skip-and-warn malformed EXR filenames (no trailing frame number)**

  **What to do**:
  - In `src/pipeline/sequence_processor.cpp`, locate the directory enumeration that builds the frame list (calls `extractTrailingFrameNumber` at line 583 per master). Currently when `extractTrailingFrameNumber` returns `false`, the frame is still added with `frameNumber = INT_MAX` and processed.
  - Change behavior: when `extractTrailingFrameNumber` returns false, log a warning (NDJSON event `{"type":"skipped_file","path":"...","reason":"no_trailing_frame_number"}`) and DO NOT add the frame to the processing list.
  - Edge case: if ALL files lack frame numbers, emit a single error event `{"type":"error","reason":"no_valid_frames","detail":"directory contains no files with trailing frame numbers"}` and exit with code != 0 instead of processing nothing silently.
  - Add unit test `tests/test_sequence_filename_filter.cpp`:
    - Synthetic dir with 5 valid files (`frame_0001.exr` through `frame_0005.exr`) and 2 invalid (`frame_.exr`, `_intermediate.exr`)
    - Assert build returns exactly 5 frames, with the invalid ones skipped (use a mock event collector to capture warnings)
    - Edge case: dir with 0 valid files → expect error event + non-zero exit
    - Edge case: dir with 1 valid file → expect single-frame processing succeeds (already supported)

  **Must NOT do**:
  - Do NOT implement Adobe-PR style "pick first frame from this filename" feature in this task — explicitly deferred
  - Do NOT change `extractTrailingFrameNumber` signature/behavior (it's a pure function — filtering is the caller's job)
  - Do NOT touch FG path
  - Do NOT silently sort INT_MAX-tagged files to end and continue

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single-file change with clear boundaries; well-defined test cases
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T1, T2, T10, T12)
  - **Parallel Group**: Wave 1
  - **Blocks**: T9 (Playwright spec for malformed file scenario)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:85` — `extractTrailingFrameNumber` definition
  - `src/pipeline/sequence_processor.cpp:583` — call site (where filtering logic goes)
  - Existing NDJSON emission helper (search for `"type":"banner"` or similar — same helper used)

  **API/Type References**:
  - `std::filesystem::directory_iterator`
  - Project NDJSON event helper

  **External References**: none

  **WHY Each Reference Matters**:
  - `extractTrailingFrameNumber:85` — confirms the function correctly returns false; the bug is downstream

  **Acceptance Criteria**:

  - [ ] `tests/test_sequence_filename_filter.cpp` covers 3 scenarios (valid + skipped, zero valid, single valid)
  - [ ] `ctest -C Release -R SequenceFilenameFilter` → PASS

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Malformed file skipped, valid frames processed
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; create fixture fixtures/malformed_test/ with frame_0001.exr through frame_0005.exr plus filename_.exr
    Steps:
      1. Run: dlss-compositor.exe --input-dir fixtures/malformed_test --output-dir tmp/malformed --scale 2 --progress-format json 2> tmp/malformed.ndjson
      2. Select-String -Path tmp/malformed.ndjson -Pattern '"type":"skipped_file"' ; assert ≥1 match
      3. Count output files in tmp/malformed/ ; assert exactly 5
    Expected Result: 1 skip event, 5 output frames
    Failure Indicators: skip event missing OR 6 output frames (means malformed processed)
    Evidence: .sisyphus/evidence/gui-rewrite/task-5-skip-warn.ndjson

  Scenario: All-invalid directory errors out
    Tool: Bash (PowerShell)
    Preconditions: Create fixture with only filename_.exr, _intermediate.exr (no valid frame names)
    Steps:
      1. Run: dlss-compositor.exe --input-dir fixtures/all_invalid --output-dir tmp/x --scale 2 --progress-format json 2> tmp/x.ndjson
      2. Verify exit code != 0
      3. Select-String -Path tmp/x.ndjson -Pattern '"reason":"no_valid_frames"'
    Expected Result: Non-zero exit + explicit error event
    Failure Indicators: Exit 0 with no output OR exit non-zero but no error event
    Evidence: .sisyphus/evidence/gui-rewrite/task-5-all-invalid.ndjson
  ```

  **Evidence to Capture**:
  - [ ] task-5-skip-warn.ndjson
  - [ ] task-5-all-invalid.ndjson
  - [ ] task-5-unit-tests.txt

  **Commit**: YES (3/16)
  - Files: `src/pipeline/sequence_processor.cpp`, `tests/test_sequence_filename_filter.cpp`, `tests/CMakeLists.txt`, `fixtures/malformed_test/`, `fixtures/all_invalid/`
  - Pre-commit: `ctest -C Release -R SequenceFilenameFilter`

- [x] 10. **Delete ExePathConfig + auto-detect dev vs packaged exe path**

  **What to do**:
  - Delete `gui/src/components/ExePathConfig.tsx` (entire file)
  - Remove its import + usage from `gui/src/App.tsx` (or wherever it's mounted)
  - Remove related state from `gui/src/state/config-store.ts` (the `exePath` field if user-settable; keep an internal resolved path field but don't persist it)
  - Create new module `gui/electron/exe-locator.ts`:
    ```typescript
    import path from 'node:path'
    import fs from 'node:fs'
    import { app } from 'electron'

    export function locateCliExecutable(): string {
      const isDev = !app.isPackaged
      // Try dev path first when in dev mode
      if (isDev) {
        const devPath = path.resolve(__dirname, '../../../build/Release/dlss-compositor.exe')
        if (fs.existsSync(devPath)) return devPath
      }
      // Try packaged location: sibling to GUI exe
      const packagedPath = path.join(path.dirname(app.getPath('exe')), 'dlss-compositor.exe')
      if (fs.existsSync(packagedPath)) return packagedPath
      // Fall back to dev path even in packaged mode (last resort)
      const fallback = path.resolve(__dirname, '../../../build/Release/dlss-compositor.exe')
      if (fs.existsSync(fallback)) return fallback
      throw new Error(
        `dlss-compositor.exe not found. Searched: ${packagedPath}, ${fallback}. ` +
        `In dev mode, build C++ first: cmake --build build --config Release`
      )
    }
    ```
  - Update `gui/electron/main.ts` to call `locateCliExecutable()` once at startup, store the resolved path in module state, use everywhere process spawns occur.
  - Error handling: if `locateCliExecutable()` throws, send IPC event to renderer to show explicit error in StatusBar (text: "CLI executable not found at expected path. Please rebuild or reinstall."). Do NOT crash main process.
  - Keep IPC channel `exe:get-resolved-path` (read-only) so StatusBar can display the active path for debugging — useful in dev mode.

  **Must NOT do**:
  - Do NOT preserve the path field as a hidden/advanced setting
  - Do NOT add a "browse for exe" fallback dialog
  - Do NOT search PATH environment variable (auto-detection should be path-explicit, not PATH-roulette)
  - Do NOT migrate existing user settings (path field is dropped on first run)
  - Do NOT touch process spawn logic in `gui/electron/process-manager.ts` beyond consuming the resolved path

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Affects multiple files (component delete, state cleanup, electron locator, IPC, main.ts wiring); needs careful state-removal without orphan references
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T1, T2, T5, T12)
  - **Parallel Group**: Wave 1
  - **Blocks**: T11 (settings store no longer carries exePath), T13 (DLSS detect uses resolved exe path)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `gui/electron/main.ts` — current process spawn location (search for `spawn(` or `execFile(`)
  - `gui/src/components/ExePathConfig.tsx` — file to delete
  - `gui/src/App.tsx` — find ExePathConfig import + JSX usage
  - `gui/electron/process-manager.ts` — already exists; do NOT modify its internal logic, only how it gets the path

  **API/Type References**:
  - Electron: `app.isPackaged`, `app.getPath('exe')`
  - Node: `path.resolve`, `fs.existsSync`

  **External References**:
  - Electron docs on `app.isPackaged`: `https://www.electronjs.org/docs/latest/api/app#appispackaged-readonly`

  **WHY Each Reference Matters**:
  - `app.isPackaged` is the canonical Electron way to differentiate dev vs prod (more reliable than `process.env.NODE_ENV`)

  **Acceptance Criteria**:

  - [ ] `Get-ChildItem gui/src -Recurse -Filter *.tsx | Select-String 'ExePathConfig'` → 0 matches
  - [ ] File `gui/src/components/ExePathConfig.tsx` deleted
  - [ ] `gui/electron/exe-locator.ts` exists, exports `locateCliExecutable`
  - [ ] GUI launches in dev mode and resolves to `build/Release/dlss-compositor.exe`
  - [ ] GUI launches in packaged mode and resolves to sibling exe

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Dev mode auto-detects dev exe
    Tool: Playwright (via playwright skill)
    Preconditions: Build succeeded (build/Release/dlss-compositor.exe exists); GUI in dev mode (npm run dev)
    Steps:
      1. Launch GUI: cd gui ; npm run dev (in background)
      2. Wait for app window
      3. Open DevTools console; evaluate: window.electronAPI.getResolvedExePath()
      4. Assert returned path ends with 'build\Release\dlss-compositor.exe'
      5. Assert no ExePathConfig UI element visible (search DOM for component)
    Expected Result: Auto-detected dev path; no path picker UI
    Evidence: .sisyphus/evidence/gui-rewrite/task-10-dev-detect.png

  Scenario: Missing exe shows clear error
    Tool: Playwright
    Preconditions: Temporarily rename build/Release/dlss-compositor.exe; clear sibling location too
    Steps:
      1. Launch GUI in dev mode
      2. Wait for StatusBar to render
      3. Assert StatusBar text contains "CLI executable not found"
      4. Assert no silent hang (status updates within 5 seconds)
    Expected Result: Explicit error message in StatusBar
    Evidence: .sisyphus/evidence/gui-rewrite/task-10-missing-error.png

  Scenario: ExePathConfig removed from DOM
    Tool: Playwright
    Preconditions: Normal launch
    Steps:
      1. Launch GUI
      2. Search DOM for any element with text "exe path", "executable path", "browse" near a path field
      3. Assert no such element
    Expected Result: Path picker UI fully gone
    Evidence: .sisyphus/evidence/gui-rewrite/task-10-no-picker.png
  ```

  **Evidence to Capture**:
  - [ ] task-10-dev-detect.png
  - [ ] task-10-missing-error.png
  - [ ] task-10-no-picker.png

  **Commit**: YES (4/16)
  - Files: `gui/src/components/ExePathConfig.tsx` (delete), `gui/electron/exe-locator.ts` (new), `gui/electron/main.ts`, `gui/electron/preload.ts`, `gui/electron/ipc-channels.ts`, `gui/src/App.tsx`, `gui/src/state/config-store.ts`
  - Pre-commit: `cd gui ; npm run typecheck`

- [x] 12. **VSCode F5 dev launch configurations**

  **What to do**:
  - Create `.vscode/launch.json` with 3 configurations:
    ```json
    {
      "version": "0.2.0",
      "configurations": [
        {
          "name": "Launch GUI (Dev)",
          "type": "node",
          "request": "launch",
          "preLaunchTask": "npm: dev:wait",
          "cwd": "${workspaceFolder}/gui",
          "runtimeExecutable": "${workspaceFolder}/gui/node_modules/.bin/electron.cmd",
          "args": [".", "--enable-logging"],
          "env": { "NODE_ENV": "development" },
          "outputCapture": "std",
          "console": "integratedTerminal"
        },
        {
          "name": "Launch GUI (Packaged)",
          "type": "node",
          "request": "launch",
          "preLaunchTask": "npm: package",
          "program": "${workspaceFolder}/gui/dist/dlss-compositor-gui.exe",
          "console": "integratedTerminal"
        },
        {
          "name": "Debug C++ CLI",
          "type": "cppvsdbg",
          "request": "launch",
          "preLaunchTask": "cmake: build release",
          "program": "${workspaceFolder}/build/Release/dlss-compositor.exe",
          "args": ["--input-dir", "${workspaceFolder}/fixtures/sintel_short", "--output-dir", "${workspaceFolder}/tmp/debug-out", "--scale", "2"],
          "cwd": "${workspaceFolder}",
          "console": "integratedTerminal"
        }
      ]
    }
    ```
  - Create `.vscode/tasks.json` with the referenced tasks:
    - `npm: dev:wait` — runs `cd gui && npm run dev`, waits for "vite" or "ready" pattern in output before exiting
    - `npm: package` — runs `cd gui && npm run package` (or whatever the existing package script is)
    - `cmake: build release` — runs `cmake --build build --config Release`
  - Verify each launch configuration works:
    - F5 with "Launch GUI (Dev)" selected → GUI window appears, can hit breakpoint in `main.ts` (set one on a known startup path like `app.whenReady`)
    - F5 with "Debug C++ CLI" → debugger attaches, can hit breakpoint in `cli_parser.cpp` `parseArgs` or similar
  - Document in `docs/development.md` (full doc written in T16; this task creates the file with sections for "Prerequisites" and "F5 Workflow"; T16 enriches)

  **Must NOT do**:
  - Do NOT add launch configs that require non-default extensions beyond the Microsoft C/C++ extension (already standard)
  - Do NOT hardcode user-specific paths (use `${workspaceFolder}` everywhere)
  - Do NOT include configs that download/install tools at launch
  - Do NOT add launch config for Blender extension debugging (out of scope; user can run `blender --debug` manually)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Configuration files only; no algorithmic complexity
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (truly independent — touches only `.vscode/`)
  - **Parallel Group**: Wave 1
  - **Blocks**: None
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - VSCode `launch.json` examples in similar repos (e.g., search github.com for `electron + cppvsdbg launch.json`)

  **API/Type References**:
  - VSCode launch schema: `https://code.visualstudio.com/docs/editor/debugging#_launch-configurations`
  - Microsoft C/C++ debugger config: `cppvsdbg` for Windows MSVC

  **External References**:
  - VSCode tasks reference: `https://code.visualstudio.com/docs/editor/tasks`

  **WHY Each Reference Matters**:
  - `cppvsdbg` is Windows-only (uses Visual Studio debugger); since project is Windows-only, this is the right choice over `cppdbg` (which uses GDB/LLDB on cross-platform)

  **Acceptance Criteria**:

  - [ ] `.vscode/launch.json` exists with 3 configurations
  - [ ] `.vscode/tasks.json` exists with referenced tasks
  - [ ] All 3 launch configs verified to work (evidence captured)
  - [ ] `docs/development.md` exists (stub OK; T16 enriches)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Launch.json passes JSON schema validation
    Tool: Bash (PowerShell)
    Preconditions: VSCode CLI available (`code --version`) OR fall back to manual JSON parse
    Steps:
      1. Run: ConvertFrom-Json (Get-Content .vscode/launch.json -Raw) ; assert no parse error
      2. Verify each configuration object has required fields: name, type, request
    Expected Result: Valid JSON, 3 configurations
    Evidence: .sisyphus/evidence/gui-rewrite/task-12-launch-json-valid.txt

  Scenario: Task references resolve
    Tool: Bash (PowerShell)
    Preconditions: tasks.json exists
    Steps:
      1. Parse launch.json → collect all preLaunchTask values
      2. Parse tasks.json → collect all task labels
      3. Assert every preLaunchTask label exists in tasks.json
    Expected Result: No unresolved task references
    Evidence: .sisyphus/evidence/gui-rewrite/task-12-tasks-resolved.txt

  Scenario: Manual F5 launch works (smoke test)
    Tool: Manual (capture VSCode screenshot)
    Preconditions: VSCode opens project; build/Release/dlss-compositor.exe exists; gui/node_modules installed
    Steps:
      1. Open VSCode in project root
      2. Select "Launch GUI (Dev)" from debug dropdown, press F5
      3. Verify GUI window appears within 30 seconds
      4. Verify VSCode shows "Debug Session Active" status
      5. Set breakpoint in gui/electron/main.ts on app.whenReady; restart debug; verify hit
    Expected Result: All 3 launch configs work
    Evidence: .sisyphus/evidence/gui-rewrite/task-12-f5-screenshot.png
  ```

  **Evidence to Capture**:
  - [ ] task-12-launch-json-valid.txt
  - [ ] task-12-tasks-resolved.txt
  - [ ] task-12-f5-screenshot.png (at minimum: VSCode showing debug session)

  **Commit**: YES (5/16)
  - Files: `.vscode/launch.json` (new), `.vscode/tasks.json` (new), `docs/development.md` (stub — T16 enriches)
  - Pre-commit: JSON validity check

- [x] 3. **`--inspect-exr` CLI flag + IPC channel for EXR header reads**

  **What to do**:
  - Add CLI flag in `src/cli/cli_parser.cpp` (parse loop): `--inspect-exr <file>` sets `config.inspectExrPath = <file>` and `config.mode = Mode::InspectExr`
  - Add `Mode::InspectExr` enum value in `src/cli/config.h` (or wherever modes live)
  - Create new module `src/inspection/exr_inspector.cpp` + `.h`:
    ```cpp
    struct ExrMetadata {
      int width;
      int height;
      std::vector<std::string> channels;
      std::string compression;  // "ZIP", "PIZ", etc.
      bool isMultiPart;
    };
    bool inspectExr(const std::filesystem::path& file, ExrMetadata& out, std::string& errorMsg);
    std::string toJson(const ExrMetadata& meta);
    ```
  - In `main()` (entry point), if `mode == InspectExr`: call `inspectExr`, print `toJson` to stdout, exit 0; on failure print error JSON to stderr, exit non-zero.
  - JSON output schema: `{"width":1920,"height":1080,"channels":["R","G","B","A","Z"],"compression":"ZIP","is_multi_part":false}`
  - Add IPC channel constant in `gui/electron/ipc-channels.ts`: `EXR_READ_HEADER = 'exr:read-header'`
  - Add IPC handler in `gui/electron/main.ts`:
    ```typescript
    ipcMain.handle(IPC_CHANNELS.EXR_READ_HEADER, async (_event, filePath: string) => {
      const exe = locateCliExecutable()  // from T10
      try {
        const { stdout } = await execFileAsync(exe, ['--inspect-exr', filePath], { timeout: 5000 })
        return { ok: true, data: JSON.parse(stdout) }
      } catch (err: any) {
        return { ok: false, error: err.message ?? String(err) }
      }
    })
    ```
  - Add preload bridge in `gui/electron/preload.ts`:
    ```typescript
    readExrHeader: (filePath: string) => ipcRenderer.invoke(IPC_CHANNELS.EXR_READ_HEADER, filePath)
    ```
  - Type the renderer-side API in the existing `window.electronAPI` declaration
  - Add unit test `tests/test_exr_inspector.cpp` covering: standard EXR, MultiLayer EXR, missing file (returns error), corrupted file (returns error)

  **Must NOT do**:
  - Do NOT use `child_process.spawn` directly in IPC handler — use `execFile` for simpler bounded execution
  - Do NOT cache EXR header results in this task (premature optimization)
  - Do NOT add a sync version of the inspector (everything async)
  - Do NOT extend the JSON schema beyond what's documented above (no "frame_rate" or other guesses)
  - Do NOT have the inspector load full pixel data — header-only

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Multi-language change (C++ inspector, TypeScript IPC); needs careful schema agreement between sides
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T11, T13)
  - **Parallel Group**: Wave 2
  - **Blocks**: T4 (camera-detector cleanup uses this IPC), T6 (resolution panel reads via this)
  - **Blocked By**: T2 (inspector reuses reader's format detection)

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:206-209` — simple no-arg flag pattern (`--test-ngx`); we need `--inspect-exr <arg>` so use the with-arg pattern (look elsewhere in the file)
  - `gui/electron/ipc-channels.ts` — existing channel constants
  - `gui/electron/preload.ts` — existing bridge pattern
  - `src/core/exr_reader.cpp` — header-reading logic (T2's extended version)

  **API/Type References**:
  - `Imf::InputFile::header()`, `Imf::Header::dataWindow()`, `Imf::ChannelList`
  - Node `child_process.execFile` (with `util.promisify`)
  - Electron `ipcMain.handle` / `ipcRenderer.invoke` (request/response style)

  **External References**: none

  **WHY Each Reference Matters**:
  - `ipcMain.handle` (vs `ipcMain.on`) is the modern pattern for request/response IPC; using `on` would require manual reply-channel correlation

  **Acceptance Criteria**:

  - [ ] `dlss-compositor.exe --inspect-exr <file>` outputs valid JSON with all required fields
  - [ ] `tests/test_exr_inspector.cpp` covers 4 cases (standard, MultiLayer, missing, corrupted)
  - [ ] `ctest -C Release -R ExrInspector` → PASS
  - [ ] IPC round-trip works (verified in QA scenario)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: CLI inspect-exr outputs valid JSON
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; fixture standard EXR exists
    Steps:
      1. Run: dlss-compositor.exe --inspect-exr fixtures/standard_exr/frame_0001.exr
      2. Capture stdout, parse as JSON: $j = ConvertFrom-Json $stdout
      3. Assert $j.width -is [int]; $j.height -is [int]; $j.channels -is [array]; $j.compression -is [string]
    Expected Result: Valid JSON with all expected fields
    Evidence: .sisyphus/evidence/gui-rewrite/task-3-cli-inspect-output.json

  Scenario: CLI inspect-exr handles missing file
    Tool: Bash (PowerShell)
    Steps:
      1. Run: dlss-compositor.exe --inspect-exr nonexistent.exr 2> tmp/err.json
      2. Verify exit code != 0
      3. Parse stderr as JSON; assert error field present
    Expected Result: Non-zero exit, error JSON on stderr
    Evidence: .sisyphus/evidence/gui-rewrite/task-3-cli-missing-file.json

  Scenario: IPC round-trip from renderer
    Tool: Playwright
    Preconditions: GUI launched; standard EXR fixture accessible
    Steps:
      1. Launch GUI (npm run dev)
      2. In DevTools console: const result = await window.electronAPI.readExrHeader('D:/path/to/fixture.exr')
      3. Assert result.ok === true; result.data.width > 0
    Expected Result: IPC returns parsed metadata
    Evidence: .sisyphus/evidence/gui-rewrite/task-3-ipc-roundtrip.png

  Scenario: IPC handles invalid path gracefully
    Tool: Playwright
    Steps:
      1. In DevTools: const result = await window.electronAPI.readExrHeader('Z:/nonexistent.exr')
      2. Assert result.ok === false; result.error is string
    Expected Result: Returns error result, not throws
    Evidence: .sisyphus/evidence/gui-rewrite/task-3-ipc-error.png
  ```

  **Evidence to Capture**:
  - [ ] task-3-cli-inspect-output.json
  - [ ] task-3-cli-missing-file.json
  - [ ] task-3-ipc-roundtrip.png
  - [ ] task-3-ipc-error.png
  - [ ] task-3-unit-tests.txt

  **Commit**: YES (6/16)
  - Files: `src/cli/cli_parser.cpp`, `src/cli/config.h`, `src/inspection/exr_inspector.cpp` (new), `src/inspection/exr_inspector.h` (new), `src/main.cpp` (mode dispatch), `gui/electron/main.ts`, `gui/electron/preload.ts`, `gui/electron/ipc-channels.ts`, `tests/test_exr_inspector.cpp`, `CMakeLists.txt` (add inspector to build)
  - Pre-commit: `ctest -C Release -R ExrInspector`

- [x] 11. **Persist settings to `gui/dlss-compositor.json`**

  **What to do**:
  - Rewrite `gui/electron/settings-store.ts` to use a fixed file path: `path.join(process.cwd(), 'dlss-compositor.json')` when in dev mode (cwd === gui/), or `path.join(path.dirname(app.getPath('exe')), 'dlss-compositor.json')` when packaged.
  - File schema (typed):
    ```typescript
    interface PersistedSettings {
      schemaVersion: 1
      input: { lastInputDir: string | null, lastOutputDir: string | null }
      sr: { scale: number, qualityMode: 'DLAA' | 'Quality' | 'Balanced' | 'Performance' | 'UltraPerformance' | 'UltraQuality',
            globalPreset: 'Auto' | 'J' | 'K' | 'L' | 'M',
            perQualityPresets: Record<QualityMode, 'Auto' | 'J' | 'K' | 'L' | 'M'>,
            sharpness: number, sourceFps: number }
      fg: { interpolationFactor: '2x' | '4x' | 'off', cameraDataPath: string | null, tonemapMode: string }
      advanced: { /* all NGX flags from Plan 1 T2; defaults match CLI defaults */ }
      common: { memoryBudgetGb: number, ngxLogLevel: string, encodeVideoOutput: string | null, fps: number }
    }
    ```
  - Read on app startup: `settingsStore.load()`. If file missing, use defaults. If schema mismatch, log warning, use defaults.
  - Write on settings change: debounced (500ms) writeFile of full state. Atomic write: write to `.tmp` then rename.
  - Provide IPC channels:
    - `settings:get` → returns current settings
    - `settings:set` → merges partial update, persists, returns new full state
    - `settings:reset` → drops file, returns defaults
  - Update `.gitignore`: add `gui/dlss-compositor.json`
  - Create `gui/dlss-compositor.example.json`: same schema, all defaults — committed to git so users have a reference template

  **Must NOT do**:
  - Do NOT use `electron-store` library if it persists to userData by default — either configure it to use absolute path, or use raw `fs.writeFile` (simpler, fewer deps)
  - Do NOT persist computed/derived state (e.g., resolved exe path, last-detected source resolution)
  - Do NOT validate every field strictly on load (warn-and-default for unknown fields, allow forward compatibility)
  - Do NOT auto-migrate from old electron-store userData location (clean break — atlas's prior persistence is forfeit; user's settings reset on first run; document in T16)
  - Do NOT commit `gui/dlss-compositor.json` (only `.example.json`)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Persistence layer touches multiple systems (file I/O, IPC, schema, gitignore); easy to introduce bugs with debouncing or atomic write
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T3, T13)
  - **Parallel Group**: Wave 2
  - **Blocks**: T9 (Playwright spec verifies persistence)
  - **Blocked By**: T10 (settings schema no longer carries exePath)

  **References**:

  **Pattern References**:
  - `gui/electron/settings-store.ts` — current implementation (probably uses electron-store)
  - `gui/electron/ipc-channels.ts` — channel naming convention

  **API/Type References**:
  - Node: `fs/promises.writeFile`, `fs/promises.readFile`, `fs/promises.rename`, `fs.existsSync`
  - Electron: `app.getPath('exe')`, `app.isPackaged`
  - TypeScript: typed schema with discriminated unions for safe parsing

  **External References**: none

  **WHY Each Reference Matters**:
  - Atomic write (`.tmp` + rename) — protects against partial-write corruption if app is killed mid-save

  **Acceptance Criteria**:

  - [ ] Settings file appears at `gui/dlss-compositor.json` after first GUI launch + change
  - [ ] Settings survive GUI restart (verified in QA scenario)
  - [ ] `.gitignore` includes the file
  - [ ] `gui/dlss-compositor.example.json` exists and is valid against schema
  - [ ] `settings:reset` IPC works (deletes file, restores defaults)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Settings persist across restart
    Tool: Playwright
    Preconditions: Clean state — gui/dlss-compositor.json deleted
    Steps:
      1. Launch GUI; verify file does NOT exist initially
      2. Change a setting (e.g., set scale to 4)
      3. Wait 1 second (past debounce); assert file exists
      4. Read file; assert sr.scale === 4
      5. Close GUI; restart
      6. Assert UI shows scale=4 on launch
    Expected Result: Setting round-trips through file
    Evidence: .sisyphus/evidence/gui-rewrite/task-11-persistence.png + task-11-file-content.json

  Scenario: Reset clears file and defaults state
    Tool: Playwright
    Steps:
      1. With non-default settings saved, click "Reset to Defaults" button (or call IPC directly)
      2. Assert file deleted
      3. Assert UI shows default values
    Expected Result: Clean reset
    Evidence: .sisyphus/evidence/gui-rewrite/task-11-reset.png

  Scenario: Atomic write resists corruption
    Tool: Bash (PowerShell)
    Steps:
      1. Manually corrupt gui/dlss-compositor.json (write half a JSON object)
      2. Launch GUI
      3. Assert GUI loads with defaults (does not crash); assert warning logged
    Expected Result: Graceful degradation on corruption
    Evidence: .sisyphus/evidence/gui-rewrite/task-11-corruption.txt

  Scenario: Example file is valid template
    Tool: Bash (PowerShell)
    Steps:
      1. Copy gui/dlss-compositor.example.json → gui/dlss-compositor.json
      2. Launch GUI
      3. Assert UI loads with example values (no validation errors)
    Expected Result: Example file is usable as starting config
    Evidence: .sisyphus/evidence/gui-rewrite/task-11-example.png
  ```

  **Evidence to Capture**:
  - [ ] task-11-persistence.png
  - [ ] task-11-file-content.json
  - [ ] task-11-reset.png
  - [ ] task-11-corruption.txt
  - [ ] task-11-example.png

  **Commit**: YES (7/16)
  - Files: `gui/electron/settings-store.ts`, `gui/electron/main.ts` (IPC handlers), `gui/electron/preload.ts`, `gui/electron/ipc-channels.ts`, `.gitignore`, `gui/dlss-compositor.example.json` (new)
  - Pre-commit: `cd gui ; npm run typecheck`

- [x] 13. **Diagnose and fix DLSS detection hang (root cause, not band-aid)**

  **What to do**:
  - **Step 1 — Diagnostic**: Before writing any fix code, the executing agent MUST run real reproduction:
    1. Build the CLI: `cmake --build build --config Release`
    2. Run manually: `build/Release/dlss-compositor.exe --test-ngx --progress-format json > tmp/diag-stdout.txt 2> tmp/diag-stderr.txt`
    3. Capture both streams' raw byte content (use `Format-Hex` to verify newline characters and encoding)
    4. Compare the actual NDJSON banner emission with what GUI's `gui/electron/main.ts:20-33` (atlas's probe) expects to parse
    5. Document findings in `.sisyphus/evidence/gui-rewrite/task-13-diagnostic-report.md` BEFORE writing any fix
  - **Step 2 — Identify root cause** from one of these likely categories:
    - (A) Banner is written to stderr while GUI listens to stdout (or vice versa)
    - (B) Banner JSON schema mismatch (GUI looks for `"type":"banner"` but CLI emits `"event":"ready"`, etc.)
    - (C) `--test-ngx` does not emit a banner at all (only emits NGX init result)
    - (D) Banner is emitted but NDJSON parser breaks on a missing/extra field
    - (E) Process exits before banner flushed (buffering issue)
  - **Step 3 — Fix at the right layer**:
    - If (A): change GUI to listen to the correct stream (or change CLI to emit on stdout)
    - If (B): unify schema — define canonical banner event in `src/cli/cli_parser.cpp` (or wherever banner is emitted) and update GUI parser to match exactly
    - If (C): add unconditional banner emission in `src/dlss/ngx_wrapper.cpp` after init success (and after init failure with `success: false` field), called from `--test-ngx` path
    - If (D): make GUI parser tolerant (skip non-banner JSON lines, look for `type === 'banner'`)
    - If (E): explicit `std::cout.flush()` after banner emission; ensure no buffering on stderr
  - **Step 4 — Sanity error state** (regardless of root cause): in `gui/electron/main.ts` probe handler, if process exits without banner received, emit IPC error event, StatusBar shows: "DLSS detection failed: process exited without banner. Check that NVIDIA driver and DLSS DLL are installed."
  - **Step 5 — NDJSON spec doc**: create `docs/ndjson_protocol.md` documenting all event types CLI emits, their schemas, and which stream they go to. Banner is one such event.
  - User explicitly said NOT to add a timeout as primary fix; only as defensive backup IF root cause is genuinely "infinite blocking" (rare). Document the decision in the diagnostic report.

  **Must NOT do**:
  - Do NOT add a timeout to `probeNgxBanner` as the only fix — that masks the problem
  - Do NOT skip the diagnostic step to "save time" — diagnosing first is mandatory
  - Do NOT change banner schema without coordinating both sides (CLI + GUI)
  - Do NOT touch `process-manager.ts` core spawn logic — only the banner-related lines
  - Do NOT introduce new IPC channels just for banner (reuse existing if any)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Diagnostic-first work requires careful root-cause reasoning across two languages and process boundaries; high cost of misdiagnosis (band-aid fix that doesn't actually fix)
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T3, T11)
  - **Parallel Group**: Wave 2
  - **Blocks**: T9 (Playwright spec verifies fix)
  - **Blocked By**: T10 (probe uses resolved exe path)

  **References**:

  **Pattern References**:
  - `D:\Github\dlss-compositor-opus-rework\gui\electron\main.ts:20-33` — atlas's broken probe (READ for reference, but base fix on master + T10 changes)
  - `gui/electron/main.ts` (master) — current state, see what banner-handling code exists
  - `gui/electron/process-manager.ts` — spawn pattern (do NOT modify core logic)
  - `gui/src/components/StatusBar.tsx` — atlas's "Detecting DLSS capabilities..." string source (line 18 in atlas)
  - `src/cli/cli_parser.cpp:206-209` — `--test-ngx` flag handling
  - `src/dlss/ngx_wrapper.cpp` — NGX init path (banner should be emitted here after init)

  **API/Type References**:
  - Node: `child_process.spawn` with `stdio: ['ignore', 'pipe', 'pipe']`, `readline.createInterface` for line-by-line parsing
  - C++: `std::cout.flush()`, `std::cerr` (typically unbuffered already)

  **External References**: none

  **WHY Each Reference Matters**:
  - atlas's probe code is the reference for "what we're replacing"; understand what it tried to do before fixing
  - master's current state may already be correct or partially correct; avoid undoing master work

  **Acceptance Criteria**:

  - [ ] Diagnostic report (`task-13-diagnostic-report.md`) committed with root-cause identification
  - [ ] Banner emission canonicalized in CLI (event has stable schema, documented in `docs/ndjson_protocol.md`)
  - [ ] GUI probe receives banner within 5 seconds in normal case
  - [ ] GUI probe shows explicit error in StatusBar (not silent hang) when CLI fails to emit banner
  - [ ] No timeout used as primary mechanism (verified by code review in F2)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Real CLI produces banner on stdout (or documented stream)
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; RTX GPU present
    Steps:
      1. Run: build/Release/dlss-compositor.exe --test-ngx --progress-format json > tmp/stdout.txt 2> tmp/stderr.txt
      2. Search both files for '"type":"banner"' (or canonical event name)
      3. Document which stream contains it
    Expected Result: Banner found in expected stream within 10 seconds (process exit)
    Evidence: .sisyphus/evidence/gui-rewrite/task-13-banner-stream.txt + tmp/stdout.txt + tmp/stderr.txt copies

  Scenario: GUI receives banner and updates StatusBar
    Tool: Playwright
    Preconditions: Build + npm dev
    Steps:
      1. Launch GUI
      2. Wait for StatusBar text change from "Detecting..." to either "Ready" / DLSS info OR error message
      3. Assert change happens within 10 seconds (not infinite hang)
      4. If success: assert text contains DLSS version or similar
    Expected Result: StatusBar updates with concrete state (success or error)
    Failure Indicators: StatusBar still shows "Detecting..." after 10 seconds
    Evidence: .sisyphus/evidence/gui-rewrite/task-13-statusbar-update.png

  Scenario: Process-exit-without-banner triggers explicit error
    Tool: Playwright (mock CLI: replace exe with a script that exits immediately)
    Preconditions: Temporarily replace dlss-compositor.exe with a stub that exits 0 with no output
    Steps:
      1. Launch GUI
      2. Wait for StatusBar
      3. Assert text contains "DLSS detection failed" or similar explicit error
    Expected Result: Explicit error, no silent hang
    Evidence: .sisyphus/evidence/gui-rewrite/task-13-process-exit-error.png

  Scenario: NDJSON protocol doc covers banner event
    Tool: Bash (PowerShell)
    Steps:
      1. Test-Path docs/ndjson_protocol.md
      2. Select-String -Path docs/ndjson_protocol.md -Pattern 'banner'
    Expected Result: Doc exists and documents banner event
    Evidence: .sisyphus/evidence/gui-rewrite/task-13-protocol-doc.txt
  ```

  **Evidence to Capture**:
  - [ ] task-13-diagnostic-report.md (root-cause analysis)
  - [ ] task-13-banner-stream.txt
  - [ ] task-13-statusbar-update.png
  - [ ] task-13-process-exit-error.png
  - [ ] task-13-protocol-doc.txt

  **Commit**: YES (8/16)
  - Files: `gui/electron/main.ts` (banner-handling lines only), `gui/src/components/StatusBar.tsx`, `src/cli/cli_parser.cpp` (banner emission consistency), `src/dlss/ngx_wrapper.cpp` (unconditional banner if needed), `docs/ndjson_protocol.md` (new), `.sisyphus/evidence/gui-rewrite/task-13-diagnostic-report.md`
  - Pre-commit: manual smoke test of banner in real GUI launch

- [x] 4. **Drop `camera.json` resolution dependency in GUI; consume EXR header IPC**

  **What to do**:
  - Open `gui/electron/camera-detector.ts:18-30` (atlas's `readSourceResolution` function). Either:
    - **Option A**: Delete the function entirely if it's only used for resolution (then update callers to use `window.electronAPI.readExrHeader` directly)
    - **Option B**: Refactor to read other camera.json fields (frame rate, lens info, etc. — those ARE legitimately in camera.json), removing only `render_width/render_height` lookup
  - Audit all GUI files for any `render_width` / `render_height` reference: `Get-ChildItem gui/src -Recurse | Select-String 'render_width|render_height'`. Each occurrence must either be removed or have a justified comment (e.g., for FG-related camera.json display).
  - In `gui/src/components/InputOutputPanel.tsx`:
    - When user selects an input directory, find the first `*.exr` file (alphabetical sort)
    - Call `window.electronAPI.readExrHeader(firstExrPath)` to get `{width, height}`
    - Store in zustand state as `sourceResolution: { width, height } | null`
    - Display source resolution prominently (e.g., "Source: 1920×1080 detected from frame_0001.exr")
    - On error (EXR header read failure): set `sourceResolution = null`, show warning "Could not detect source resolution. Resolution panel will fall back to manual input."
  - Update `gui/src/state/config-store.ts`:
    - Add `sourceResolution: { width: number, height: number } | null` field
    - Add action `setSourceResolution(res)` and `clearSourceResolution()`

  **Must NOT do**:
  - Do NOT keep a fallback path that reads camera.json's `render_width/height` if EXR detection fails — fall back to "unknown" UI state instead
  - Do NOT auto-pick a hardcoded default (e.g., "1920×1080 if unknown")
  - Do NOT make the IPC call synchronous (everything async; show "Detecting..." state during call)
  - Do NOT modify `camera.json` schema or C++ FG path (which still uses these fields)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Surgical refactor with clear scope; minimal logic change, mostly delete + redirect
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T6, T14, T15)
  - **Parallel Group**: Wave 3
  - **Blocks**: T6 (resolution panel reads `sourceResolution` state)
  - **Blocked By**: T3 (needs `readExrHeader` IPC)

  **References**:

  **Pattern References**:
  - atlas reference: `D:\Github\dlss-compositor-opus-rework\gui\electron\camera-detector.ts:18-30` — the function to gut
  - atlas reference: `D:\Github\dlss-compositor-opus-rework\gui\src\components\InputOutputPanel.tsx:36-38` — usage site
  - `gui/electron/camera-detector.ts` (master) — current state to modify

  **API/Type References**:
  - `window.electronAPI.readExrHeader` — from T3
  - zustand store actions in `config-store.ts`

  **External References**: none

  **WHY Each Reference Matters**:
  - atlas's specific lines pinpoint the bug; master may differ slightly so verify before edit

  **Acceptance Criteria**:

  - [ ] `Get-ChildItem gui/src -Recurse | Select-String 'render_width|render_height'` returns 0 matches (or only matches in non-resolution-detection code paths with explicit comments)
  - [ ] InputOutputPanel shows source resolution from EXR header
  - [ ] Warning appears when EXR header read fails
  - [ ] `sourceResolution` state correctly populated/cleared

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Source resolution detected from EXR
    Tool: Playwright
    Preconditions: GUI launched; fixtures/sintel_short/ has frame_0001.exr at known dims (e.g., 1920x1080)
    Steps:
      1. Launch GUI; click input dir picker; select fixtures/sintel_short/
      2. Wait for resolution display
      3. Assert UI shows "Source: 1920×1080" or equivalent
    Expected Result: Detected from EXR header, not camera.json
    Evidence: .sisyphus/evidence/gui-rewrite/task-4-detected.png

  Scenario: Camera.json resolution mismatch is ignored
    Tool: Playwright
    Preconditions: Create fixture where camera.json says 4096×4096 but actual EXR is 1920×1080
    Steps:
      1. Select that input dir
      2. Assert UI shows 1920×1080 (from EXR), NOT 4096×4096 (from camera.json)
    Expected Result: EXR truth wins
    Evidence: .sisyphus/evidence/gui-rewrite/task-4-mismatch.png

  Scenario: Missing EXR shows warning
    Tool: Playwright
    Preconditions: Empty directory or directory with only non-EXR files
    Steps:
      1. Select that input dir
      2. Assert UI shows warning text (e.g., "Could not detect source resolution")
      3. Assert ResolutionPanel switches to manual mode (verified in T6)
    Expected Result: Explicit warning
    Evidence: .sisyphus/evidence/gui-rewrite/task-4-no-exr.png

  Scenario: Code grep confirms camera.json resolution reads removed
    Tool: Bash (PowerShell)
    Steps:
      1. Get-ChildItem gui/src,gui/electron -Recurse -Include *.ts,*.tsx | Select-String 'render_width|render_height'
      2. Assert results either empty OR each match has an adjacent comment justifying it (FG path display, etc.)
    Expected Result: No spurious uses
    Evidence: .sisyphus/evidence/gui-rewrite/task-4-grep.txt
  ```

  **Evidence to Capture**:
  - [ ] task-4-detected.png
  - [ ] task-4-mismatch.png
  - [ ] task-4-no-exr.png
  - [ ] task-4-grep.txt

  **Commit**: YES (9/16)
  - Files: `gui/electron/camera-detector.ts`, `gui/src/components/InputOutputPanel.tsx`, `gui/src/state/config-store.ts`
  - Pre-commit: `cd gui ; npm run typecheck`

- [x] 6. **ResolutionPanel rewrite — aspect-locked, orientation-aware presets, bidirectional scale**

  **What to do**:
  - Rewrite `gui/src/components/ResolutionPanel.tsx` from scratch. Reference (only as anti-pattern): `D:\Github\dlss-compositor-opus-rework\gui\src\components\ResolutionPanel.tsx:59-89`.
  - Component contract:
    - Reads `sourceResolution` from store (set by T4)
    - Outputs `targetResolution: { width: number, height: number }` and `scale: number` to store (kept in sync internally)
  - Behavior:
    - **If `sourceResolution` is null** (unknown): display warning + show only a manual `scale` number input. Disable preset dropdown. When scale changes, output `targetWidth = null`, `targetHeight = null` (CLI will accept `--scale` only, no explicit dims).
    - **If `sourceResolution` is set**:
      - Compute aspect ratio: `aspect = source.width / source.height`
      - Detect orientation: landscape if `aspect > 1.0`, portrait if `< 1.0`, square if `=== 1.0`
      - Show **single input** with toggle "Width / Height" (default to whichever dimension is larger). User edits one number; the other is computed via `aspect`, floored.
      - Show **scale display + input**: `scale = max(targetWidth/sourceWidth, targetHeight/sourceHeight)` (they should be equal due to aspect lock; use width). Editing scale recomputes both width and height (floored).
      - Show **preset dropdown** populated based on orientation:
        - Landscape: `[Same as source, 1080p (1920×1080), 1440p (2560×1440), 4K (3840×2160)]`
        - Portrait: `[Same as source, 1080p (1080×1920), 1440p (1440×2560), 4K (2160×3840)]`
        - Square: `[Same as source, 1080² (1080×1080), 1440² (1440×1440), 4K² (2160×2160)]`
        - Selecting a preset sets target dims directly (and computed scale shown)
        - When user manually edits W/H/scale, dropdown shows "Custom"
      - **Bidirectional sync**: implement via single source-of-truth state (e.g., always store `targetWidth`, derive everything else). Use `useEffect` only for input → state transitions; computed values are pure derivations in render.
      - **Floor mode**: `Math.floor()` for the computed dimension to avoid fractional pixels. Visualize this with a hint when fractional values are involved (e.g., "Computed height = floor(1920 / 16 * 9) = 1080").
      - **Integer enforcement**: input fields accept only positive integers (HTML `type="number" min="1" step="1"`); on blur, parse + floor + reject NaN.
  - Update `gui/src/state/config-store.ts` to support the new state shape if needed (likely already has `targetResolution` and `scale`; ensure single-source-of-truth, no dual-state drift).
  - Visual: clean, compact layout. Use existing component design system (look at other panels in `gui/src/components/` for style references).

  **Must NOT do**:
  - Do NOT add a "lock aspect ratio" toggle — locked is the only mode
  - Do NOT use a slider for scale (user wants precise number input)
  - Do NOT auto-update target when source changes if user has manual values set (preserve user intent; only auto-update via explicit "Reset to source" button)
  - Do NOT add ratio-presets like "16:9, 4:3, 1:1" — aspect comes from source, period
  - Do NOT split the component into 5+ subcomponents to "modularize" — single ResolutionPanel.tsx is fine for this scope (anti-AI-slop)
  - Do NOT use a context provider just for this panel (local state OK)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: UX-heavy task; bidirectional sync, layout, visual design system adherence
  - **Skills**: none required (no specific frontend skill installed)

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T4, T14, T15)
  - **Parallel Group**: Wave 3
  - **Blocks**: T7 (SR settings consume target resolution), T9 (Playwright spec)
  - **Blocked By**: T3 (sourceResolution from IPC), T4 (sourceResolution in state)

  **References**:

  **Pattern References**:
  - atlas anti-pattern: `D:\Github\dlss-compositor-opus-rework\gui\src\components\ResolutionPanel.tsx:59-89` (independent W/H — DO NOT replicate)
  - Other GUI panels (e.g., master `gui/src/components/InputOutputPanel.tsx`) — for visual style consistency
  - `gui/src/state/config-store.ts` — zustand store pattern

  **API/Type References**:
  - React 18+ hooks: `useEffect`, `useState`, `useMemo`, `useCallback`
  - TypeScript strict mode (no `any`)

  **External References**:
  - WCAG numeric input guidance (for accessibility)

  **WHY Each Reference Matters**:
  - atlas's W/H independence is exactly what we're avoiding; reading the broken implementation clarifies the contract we want

  **Acceptance Criteria**:

  - [ ] User cannot construct a frame with mismatched aspect (verified by Playwright)
  - [ ] Source-unknown state correctly shows warning + manual scale input
  - [ ] Preset dropdown adjusts based on detected orientation
  - [ ] Bidirectional sync: edit W → H + scale update; edit H → W + scale update; edit scale → W + H update
  - [ ] All computed values use `Math.floor`
  - [ ] No `any` types; passes typecheck
  - [ ] No "lock aspect" toggle in UI

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Aspect lock prevents mismatched W/H
    Tool: Playwright
    Preconditions: GUI launched; source 1920×1080 detected (aspect 1.78)
    Steps:
      1. In ResolutionPanel, current default — verify W=1920, H=1080
      2. Edit W input to 3840; assert H auto-updates to 2160 (3840/1.778 floored)
      3. Try to edit H independently — assert input is locked OR re-edit H to 800: verify W is recomputed accordingly (W = 800 * 1.778 = 1422)
    Expected Result: Aspect always preserved
    Failure Indicators: H stays at 2160 when W changes; OR independent H input accepted
    Evidence: .sisyphus/evidence/gui-rewrite/task-6-aspect-lock.png + .webm video

  Scenario: Bidirectional scale sync
    Tool: Playwright
    Preconditions: Source 1920×1080
    Steps:
      1. Edit scale to 2.0; assert W=3840, H=2160
      2. Edit W to 5760; assert scale auto-updates to 3.0
      3. Edit H to 1080; assert scale=1.0, W=1920
    Expected Result: Three-way sync among W, H, scale
    Evidence: .sisyphus/evidence/gui-rewrite/task-6-bidirectional.webm

  Scenario: Orientation-aware presets
    Tool: Playwright
    Preconditions: First with 1920×1080 source, then 1080×1920 (portrait)
    Steps:
      1. Landscape source → click preset dropdown → assert items include "4K (3840×2160)"
      2. Switch to portrait fixture → assert items include "4K (2160×3840)"
    Expected Result: Presets adapt to orientation
    Evidence: .sisyphus/evidence/gui-rewrite/task-6-orientation.png

  Scenario: Source-unknown fallback
    Tool: Playwright
    Preconditions: Empty input dir → no source detected
    Steps:
      1. Verify warning text appears in panel
      2. Verify only "Scale" input is enabled; W/H/preset disabled
      3. Edit scale to 1.5; verify state updates targetScale (no targetWidth/Height set)
    Expected Result: Graceful manual fallback
    Evidence: .sisyphus/evidence/gui-rewrite/task-6-unknown.png

  Scenario: Floor mode handles fractional aspect
    Tool: Playwright
    Preconditions: Source 1921×1080 (deliberately weird aspect ~1.7787)
    Steps:
      1. Edit W to 1920; verify H = floor(1920 / 1.7787) = 1079
    Expected Result: Floor applied, no fractional
    Evidence: .sisyphus/evidence/gui-rewrite/task-6-floor.png
  ```

  **Evidence to Capture**:
  - [ ] task-6-aspect-lock.png + .webm
  - [ ] task-6-bidirectional.webm
  - [ ] task-6-orientation.png
  - [ ] task-6-unknown.png
  - [ ] task-6-floor.png

  **Commit**: YES (10/16)
  - Files: `gui/src/components/ResolutionPanel.tsx`, `gui/src/state/config-store.ts`
  - Pre-commit: `cd gui ; npm run typecheck && npm run lint`

- [x] 14. **FFmpeg preview structured errors + GUI degradation**

  **What to do**:
  - Open `gui/electron/preview-generator.ts:9-50` (atlas's silent-failure version) and current master version. Refactor to:
    ```typescript
    type PreviewResult =
      | { success: true; dataUrl: string }  // base64 data URL for display
      | { success: false; reason: 'ffmpeg-not-found' | 'exr-decode-error' | 'timeout' | 'unknown'; detail: string }

    export async function generatePreview(exrPath: string, opts?: { timeoutMs?: number }): Promise<PreviewResult> {
      const ffmpeg = await locateFFmpeg()  // try PATH; return null if not found
      if (!ffmpeg) return { success: false, reason: 'ffmpeg-not-found', detail: 'FFmpeg not found in PATH' }

      const child = spawn(ffmpeg, ['-y', '-i', exrPath, '-frames:v', '1', '-f', 'image2pipe', '-vcodec', 'png', 'pipe:1'])
      // collect stdout chunks (PNG bytes), stderr chunks (error text)
      // on close: if exit 0 → encode stdout as base64 data URL; else inspect stderr
      // on timeout (default 10s): kill child, return timeout reason
    }
    ```
  - Add `locateFFmpeg()`: try `ffmpeg --version` via execFile, return path on success, null on ENOENT
  - Wire IPC channel `preview:generate` → returns `PreviewResult`
  - In `gui/src/components/ProcessingView.tsx` (preview area only — full simplification is T15), based on `result.reason`:
    - `ffmpeg-not-found`: show banner with text "FFmpeg not found in PATH. Install FFmpeg from https://ffmpeg.org for live preview." + clickable link
    - `exr-decode-error`: show "Frame format not supported by FFmpeg. Output preview unavailable."
    - `timeout`: show "Preview generation timed out. Frame may be corrupt or very large."
    - `unknown`: show "Preview failed. Details: {detail}"
  - On startup: probe FFmpeg once and emit IPC `system:ffmpeg-status` with availability — UI can show top-level banner if missing
  - Add unit-style test (Node test runner or simple mock-based test): mock spawn to return various exit codes/stderr strings; assert correct `reason` returned

  **Must NOT do**:
  - Do NOT throw exceptions from `generatePreview` — always return a structured result
  - Do NOT use `null` as a sentinel for failure — always return a discriminated union
  - Do NOT remove FFmpeg dependency entirely (still needed for video encoding too)
  - Do NOT add a custom EXR-to-PNG converter to avoid FFmpeg (out of scope)
  - Do NOT cache FFmpeg path indefinitely (re-probe on launch in case user installs FFmpeg between runs; once per launch is fine)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Subprocess + error-state + UI integration; multiple failure modes to handle correctly
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T4, T6, T15)
  - **Parallel Group**: Wave 3
  - **Blocks**: T9, T15 (T15 may simplify the preview area; coordinate)
  - **Blocked By**: T11 (uses settings for any config like custom FFmpeg path; though no such setting required, having store available simplifies)

  **References**:

  **Pattern References**:
  - atlas reference: `D:\Github\dlss-compositor-opus-rework\gui\electron\preview-generator.ts:9-50` — silent failure pattern (anti-pattern)
  - master `gui/electron/preview-generator.ts` — current state
  - `gui/electron/process-manager.ts` — spawn pattern (don't modify, but reference style)

  **API/Type References**:
  - Node `child_process.spawn` with stdio piping
  - Node `Buffer.concat`, `Buffer.toString('base64')`

  **External References**:
  - FFmpeg image2pipe docs: `https://ffmpeg.org/ffmpeg-formats.html#image2pipe`

  **WHY Each Reference Matters**:
  - `image2pipe` outputs raw image bytes to stdout — clean way to get PNG without temp file

  **Acceptance Criteria**:

  - [ ] `generatePreview` always returns `PreviewResult` (no throws, no null)
  - [ ] All 4 reason codes reachable in tests
  - [ ] GUI shows distinct messages for each reason
  - [ ] Top-level FFmpeg-missing banner appears on startup if FFmpeg not in PATH
  - [ ] No `console.log` debug spam left in production code

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: FFmpeg available — preview generated
    Tool: Playwright
    Preconditions: GUI launched; FFmpeg in PATH; valid EXR fixture
    Steps:
      1. Trigger preview generation (e.g., select first frame in input dir)
      2. Wait for preview area to update
      3. Assert image element has data: URL with non-empty content
    Expected Result: Preview image shown
    Evidence: .sisyphus/evidence/gui-rewrite/task-14-success.png

  Scenario: FFmpeg missing — explicit banner shown
    Tool: Playwright
    Preconditions: Temporarily remove FFmpeg from PATH (rename, or spawn GUI with modified PATH)
    Steps:
      1. Launch GUI
      2. Wait for FFmpeg status check
      3. Assert top-level banner contains "FFmpeg not found"
      4. Trigger preview generation
      5. Assert preview area shows install instructions, NOT silent null
    Expected Result: Clear user guidance
    Evidence: .sisyphus/evidence/gui-rewrite/task-14-no-ffmpeg.png

  Scenario: Corrupted EXR — exr-decode-error reason
    Tool: Playwright
    Preconditions: Place a corrupted .exr (random bytes) in fixture dir
    Steps:
      1. Trigger preview for that file
      2. Assert UI shows "Frame format not supported" or similar
    Expected Result: Decode-error reason surfaced
    Evidence: .sisyphus/evidence/gui-rewrite/task-14-corrupt.png

  Scenario: Timeout — preview cancelled cleanly
    Tool: Playwright
    Preconditions: Mock FFmpeg with a script that hangs forever
    Steps:
      1. Trigger preview with timeoutMs=2000 (override for test)
      2. Wait 3 seconds
      3. Assert UI shows timeout message
      4. Verify no zombie ffmpeg process (Get-Process -Name ffmpeg | Should -BeEmpty)
    Expected Result: Timeout caught, child killed
    Evidence: .sisyphus/evidence/gui-rewrite/task-14-timeout.png
  ```

  **Evidence to Capture**:
  - [ ] task-14-success.png
  - [ ] task-14-no-ffmpeg.png
  - [ ] task-14-corrupt.png
  - [ ] task-14-timeout.png

  **Commit**: YES (11/16)
  - Files: `gui/electron/preview-generator.ts`, `gui/electron/main.ts` (FFmpeg probe + IPC), `gui/electron/preload.ts`, `gui/electron/ipc-channels.ts`, `gui/src/components/ProcessingView.tsx` (error display only — full simplification is T15), `gui/tests/unit/preview-generator.test.ts` (if a unit test runner is set up; else covered by Playwright)
  - Pre-commit: `cd gui ; npm run typecheck`

- [x] 15. **ProcessingView simplification — preview-only, remove duplicate progress**

  **What to do**:
  - Open `gui/src/components/ProcessingView.tsx` (master + atlas versions). Identify the duplicate progress bar(s) — likely both an inline progress within ProcessingView AND a top-level progress in StatusBar or App layout.
  - User explicitly said: "右边栏上面已经有进度了, 其实只显示预览就行了" → keep top-level progress, strip ProcessingView down to preview-only.
  - Refactored layout:
    - **ProcessingView** (right column): just the preview image area + frame number indicator (e.g., "Frame 42 / 200"). No progress bar inside this component.
    - **Top-level progress** (likely StatusBar or a top banner): owns the percentage/bar/ETA display. If currently in StatusBar, leave there; if not, promote.
  - Cross-check `gui/src/App.tsx` layout — verify left/center column shows main progress; right column shows only preview after the change.
  - Visual: preview area gets more vertical space (since progress UI is removed). Make preview maintain aspect ratio of source resolution. Frame number overlay or below image.
  - Coordinate with T14: error states from preview-generator are still rendered here (banners or placeholder image area) — but no progress bar in this region.

  **Must NOT do**:
  - Do NOT remove the top-level progress bar (the one in StatusBar / center column) — only the duplicate inside ProcessingView
  - Do NOT add new progress display (e.g., ring, gauge) — single existing top-level bar is the source of truth
  - Do NOT change progress data flow / state (purely a UI layout change)
  - Do NOT redesign App.tsx layout beyond this consolidation
  - Do NOT introduce a fullscreen preview mode (out of scope; future enhancement)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: UI layout work; needs visual judgment about space allocation and component boundaries
  - **Skills**: none required

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T4, T6, T14)
  - **Parallel Group**: Wave 3
  - **Blocks**: T9 (Playwright spec verifies layout)
  - **Blocked By**: T6 (only loosely — both might touch grid layout; sequence Wave 3 carefully if conflicts arise, but expected independent)

  **References**:

  **Pattern References**:
  - `gui/src/components/ProcessingView.tsx` (master) — current implementation
  - `gui/src/components/StatusBar.tsx` — where top-level progress likely lives
  - `gui/src/App.tsx` — overall layout (grid/flexbox setup)

  **API/Type References**:
  - React layout (CSS modules or styled-components — match existing project style)

  **External References**: none

  **WHY Each Reference Matters**:
  - StatusBar likely already has the canonical progress; verify before deduplicating

  **Acceptance Criteria**:

  - [ ] `ProcessingView.tsx` no longer renders any `<ProgressBar>` or progress-related UI element
  - [ ] Top-level progress remains functional (verified by Playwright)
  - [ ] Preview area takes more vertical space than before
  - [ ] Frame number display present (somewhere — StatusBar or near preview)
  - [ ] Layout looks balanced (visual comparison vs before/after screenshots)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: No duplicate progress bars
    Tool: Playwright
    Preconditions: GUI launched; processing started (or simulated processing state)
    Steps:
      1. Start a processing run (or set zustand state to in-progress with mock)
      2. Query DOM for all elements with role="progressbar" OR class containing "progress"
      3. Assert exactly one such element (the top-level one)
    Expected Result: Single progress UI
    Failure Indicators: 2+ progress elements
    Evidence: .sisyphus/evidence/gui-rewrite/task-15-single-progress.png + DOM snapshot

  Scenario: Preview area expanded
    Tool: Playwright (visual measurement)
    Preconditions: GUI launched
    Steps:
      1. Take screenshot before commit (baseline) — actually use the master branch as baseline
      2. Take screenshot after commit
      3. Measure pixel height of preview container (via element.getBoundingClientRect())
      4. Assert post-commit height > baseline height by ≥20%
    Expected Result: More room for preview
    Evidence: .sisyphus/evidence/gui-rewrite/task-15-before.png + task-15-after.png

  Scenario: Frame number visible during processing
    Tool: Playwright
    Steps:
      1. Trigger processing
      2. Wait for at least one progress event
      3. Assert text matching pattern "Frame \d+ / \d+" visible somewhere in viewport
    Expected Result: Frame counter visible
    Evidence: .sisyphus/evidence/gui-rewrite/task-15-frame-counter.png
  ```

  **Evidence to Capture**:
  - [ ] task-15-single-progress.png
  - [ ] task-15-before.png + task-15-after.png
  - [ ] task-15-frame-counter.png

  **Commit**: YES (12/16)
  - Files: `gui/src/components/ProcessingView.tsx`, `gui/src/components/StatusBar.tsx` (if progress consolidated here), `gui/src/App.tsx` (if layout adjusted)
  - Pre-commit: `cd gui ; npm run typecheck && npm run lint`

- [x] 7. **Split BasicSettings into SRSettings + FGSettings + CommonSettings**

  **What to do**:
  - Open `gui/src/components/BasicSettings.tsx` (atlas reference: lines 34-106). Identify which controls belong to SR, FG, or both.
  - Create three new components:
    - **`gui/src/components/CommonSettings.tsx`**: input dir, output dir, memory budget (Gb), output passes selection, video encoding (encode-video output, FPS), ngx-log-level
    - **`gui/src/components/SRSettings.tsx`**: scale (delegates to ResolutionPanel from T6), quality mode (`DLAA / Quality / Balanced / Performance / UltraPerformance / UltraQuality`), global preset, source-fps, dlss-sharpness. Embeds ResolutionPanel.
    - **`gui/src/components/FGSettings.tsx`**: interpolation factor (`off / 2x / 4x`), camera-data file picker (camera.json), tonemap mode (`pq / scene-linear / none`), forward LUT file picker, inverse LUT file picker, no-inverse-tonemap checkbox
  - Each component:
    - Uses zustand store slices (e.g., `useConfigStore(state => state.sr)`)
    - Has clear section header (`<h2>SR Settings</h2>` etc.)
    - Visually distinct (subtle border or panel background) so user knows the grouping
  - Delete `gui/src/components/BasicSettings.tsx` (do NOT keep as stub re-exporting). Force callers to import from new files.
  - Update `gui/src/App.tsx` layout to render the three components (Tab UI or Accordion or simply stacked sections — pick stacked for simplicity unless project already has Tab convention). Order: Common → SR → FG (Common first because input/output is the entry point).
  - Update `gui/src/state/config-store.ts` to organize state into three slices (`sr`, `fg`, `common`) matching component scope. Migrate any flat fields into the appropriate slice. Update `cli-args.ts` (Task 8 also touches this) accordingly.
  - Persistence (T11) updated to match new schema (sr/fg/common keys at top level)

  **Must NOT do**:
  - Do NOT keep `BasicSettings.tsx` as a thin wrapper — delete it
  - Do NOT use react-router or any new routing for tab switching (use simple state if Tab UI is chosen)
  - Do NOT introduce a settings-context provider (zustand is already the global store)
  - Do NOT add settings categories beyond Common/SR/FG (no "Profiles", "Presets", "Advanced" at this level — Advanced is a sub-section of SR per T8)
  - Do NOT change CLI flag names — only group their UI representation

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Multi-component split with state restructuring and visual layout — UX-heavy
  - **Skills**: none required

  **Parallelization**:
  - **Can Run In Parallel**: NO (alone in Wave 4 with T8, but T8 depends on T7)
  - **Parallel Group**: Wave 4
  - **Blocks**: T8 (Advanced sub-panel attaches to SRSettings), T9
  - **Blocked By**: T6 (SRSettings embeds ResolutionPanel)

  **References**:

  **Pattern References**:
  - atlas reference: `D:\Github\dlss-compositor-opus-rework\gui\src\components\BasicSettings.tsx:34-106` — current monolith
  - master `gui/src/components/BasicSettings.tsx` — current state
  - `gui/src/state/config-store.ts` — state shape

  **API/Type References**:
  - zustand slices pattern: `https://zustand.docs.pmnd.rs/guides/slices-pattern`

  **External References**: none

  **WHY Each Reference Matters**:
  - zustand slices pattern keeps related state co-located with related actions; helps with the SR/FG/Common split

  **Acceptance Criteria**:

  - [ ] `BasicSettings.tsx` deleted; no references remain
  - [ ] Three new components exist and render distinct sections
  - [ ] State organized into `sr / fg / common` slices
  - [ ] App.tsx renders three sections in order (Common, SR, FG)
  - [ ] No TypeScript errors

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Three distinct sections visible
    Tool: Playwright
    Preconditions: GUI launched
    Steps:
      1. Take full-page screenshot
      2. Assert headers "SR Settings" and "FG Settings" and "Common Settings" (or equivalent labels) all visible
      3. Visually verify sections are grouped (use DOM inspection: assert each section is a distinct container)
    Expected Result: Clear separation
    Evidence: .sisyphus/evidence/gui-rewrite/task-7-three-sections.png

  Scenario: Settings independent — changing SR doesn't affect FG
    Tool: Playwright
    Steps:
      1. Set SR scale to 4
      2. Verify FG interpolation factor unchanged
      3. Set FG to 2x
      4. Verify SR scale still 4
    Expected Result: Independent state
    Evidence: .sisyphus/evidence/gui-rewrite/task-7-independent.png

  Scenario: BasicSettings deleted — no orphan imports
    Tool: Bash (PowerShell)
    Steps:
      1. Test-Path gui/src/components/BasicSettings.tsx ; assert False
      2. Get-ChildItem gui/src -Recurse | Select-String 'BasicSettings' ; assert no matches
    Expected Result: Clean removal
    Evidence: .sisyphus/evidence/gui-rewrite/task-7-no-orphan.txt

  Scenario: CLI args still work
    Tool: Playwright
    Steps:
      1. Configure SR scale + FG 2x + camera-data path
      2. Trigger "Show CLI command" feature (if exists) OR inspect built args via DevTools
      3. Assert args include --scale 2 --interpolate 2x --camera-data /path
    Expected Result: Args correctly built from new state structure
    Evidence: .sisyphus/evidence/gui-rewrite/task-7-args.txt
  ```

  **Evidence to Capture**:
  - [ ] task-7-three-sections.png
  - [ ] task-7-independent.png
  - [ ] task-7-no-orphan.txt
  - [ ] task-7-args.txt

  **Commit**: YES (13/16)
  - Files: `gui/src/components/BasicSettings.tsx` (delete), `gui/src/components/CommonSettings.tsx` (new), `gui/src/components/SRSettings.tsx` (new), `gui/src/components/FGSettings.tsx` (new), `gui/src/App.tsx`, `gui/src/state/config-store.ts`, `gui/electron/cli-args.ts`
  - Pre-commit: `cd gui ; npm run typecheck && npm run lint`

- [x] 8. **Advanced sub-panel — expose all Plan 1 NGX flags in GUI**

  **What to do**:
  - Create `gui/src/components/AdvancedNgxPanel.tsx`. Render inside `SRSettings.tsx` as a collapsible section (default collapsed): `<details><summary>Advanced NGX Settings (Power User)</summary>...</details>`
  - Expose every flag from Plan 1 Task 2 + Plan 1 Task 1 (per-quality presets) + Plan 1 Task 3 (camera cut) + Plan 1 Task 4 (banner toggles if any):
    - **Per-quality presets** (6 dropdowns): preset-dlaa, preset-quality, preset-balanced, preset-performance, preset-ultra-performance, preset-ultra-quality. Options: `Auto / J / K / L / M`. Tooltip on each: "Auto uses NVIDIA recommendation: K for DLAA/Quality/Balanced/UltraQuality, M for Performance, L for UltraPerformance"
    - **Feature flags** (3 checkboxes): feature-flag-hdr, feature-flag-mv-low-res, feature-flag-auto-exposure. All default ON. Tooltip explaining each.
    - **Output subrects** (checkbox): enable-output-subrects. Default OFF.
    - **Numeric inputs**: exposure-scale (default 1.0, range 0.0-100.0), frame-time-delta-ms (default -1 = auto, range -1.0 or > 0.0), dlss-sharpness (default 0.0, range 0.0-1.0)
    - **FG-related** (in FGSettings.tsx, not here): enable-dynamic-resolution checkbox (per handoff this is FG-related)
    - **Camera cut detection** (in SRSettings advanced): camera-cut-detection-enabled checkbox (default ON), camera-cut-translation-threshold (default 5.0), camera-cut-rotation-threshold (default 30.0)
  - Each control connects to zustand `advanced` slice
  - Update `gui/electron/cli-args.ts` to translate advanced state into CLI args. Pattern: only emit flag when value differs from default (avoid command-line bloat).
  - Visual: tight grid layout for per-quality presets; group related flags with subheaders; help icons (ⓘ) with tooltips; use the project's design system for inputs (no custom styling that breaks consistency)

  **Must NOT do**:
  - Do NOT expose flags Plan 1 didn't add — this is exact mirroring, not creative extension
  - Do NOT add validation beyond what CLI does (CLI is the source of truth; UI just hands off)
  - Do NOT add "Apply" button — changes flow to state immediately, persistence (T11) handles save
  - Do NOT default any feature flag to OFF (would change behavior — Plan 1 mandates default-preserving)
  - Do NOT skip tooltips/help text — power-user surface NEEDS documentation inline; otherwise it's just a wall of cryptic flags
  - Do NOT collapse the advanced section so deeply nested users can't find it (one collapse layer max)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Many controls × many CLI flags × careful default handling; mostly mechanical but error-prone (typos in flag names cause silent breakage)
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: NO (Wave 4 with T7; depends on T7)
  - **Parallel Group**: Wave 4
  - **Blocks**: T9
  - **Blocked By**: T7 (extends SRSettings)

  **References**:

  **Pattern References**:
  - Plan 1 file: `.sisyphus/plans/dlss-pipeline-fix.md` Tasks 1-4 — authoritative flag list
  - `gui/electron/cli-args.ts` — flag-string emission pattern
  - `gui/src/components/SRSettings.tsx` (from T7) — host component

  **API/Type References**:
  - HTML `<details>` / `<summary>` for collapse (no library needed)
  - Existing form input components

  **External References**:
  - WAI-ARIA tooltip pattern (for help icons)

  **WHY Each Reference Matters**:
  - Plan 1's task list is the **only** valid source for flag names — any divergence will silently break CLI execution

  **Acceptance Criteria**:

  - [ ] Every flag from Plan 1 Tasks 1-4 has a corresponding GUI control (verified by table comparison)
  - [ ] Defaults match CLI defaults exactly
  - [ ] Tooltip/help text present on every control
  - [ ] Changes flow to CLI args correctly (verified by inspecting computed args)
  - [ ] Advanced section collapsed by default; expandable

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: All Plan 1 flags exposed
    Tool: Playwright + Bash (cross-check)
    Preconditions: GUI launched; SR settings expanded; Advanced section opened
    Steps:
      1. From the GUI DOM, list all control labels under Advanced
      2. From Plan 1 markdown, extract all CLI flag names (grep for `--` patterns)
      3. Cross-reference: every flag has a UI control
    Expected Result: 1:1 mapping (with documented exceptions like enable-dynamic-resolution being in FG panel)
    Evidence: .sisyphus/evidence/gui-rewrite/task-8-flag-coverage.txt

  Scenario: Default values match CLI defaults
    Tool: Playwright
    Steps:
      1. Open Advanced section on fresh GUI (no settings file)
      2. For each control, read current value
      3. Compare to known defaults (e.g., feature-flag-hdr = true, exposure-scale = 1.0)
    Expected Result: All defaults correct
    Evidence: .sisyphus/evidence/gui-rewrite/task-8-defaults.png

  Scenario: Changes propagate to CLI args
    Tool: Playwright
    Steps:
      1. Set dlss-sharpness to 0.7
      2. Toggle feature-flag-auto-exposure off
      3. Set preset-performance to L (override default M)
      4. Inspect built CLI args (via DevTools or "Show CLI" UI)
      5. Assert args include --dlss-sharpness 0.7 --no-feature-flag-auto-exposure --preset-performance L
      6. Assert args do NOT include defaults (e.g., no --feature-flag-hdr because still true)
    Expected Result: Only non-default flags emitted
    Evidence: .sisyphus/evidence/gui-rewrite/task-8-args.txt

  Scenario: Tooltip helps user understand each flag
    Tool: Playwright
    Steps:
      1. Hover over help icon next to "Exposure Scale"
      2. Assert tooltip appears with explanation
      3. Repeat for 3-4 other flags
    Expected Result: Tooltips render, contain meaningful text (not lorem ipsum)
    Evidence: .sisyphus/evidence/gui-rewrite/task-8-tooltips.png
  ```

  **Evidence to Capture**:
  - [ ] task-8-flag-coverage.txt
  - [ ] task-8-defaults.png
  - [ ] task-8-args.txt
  - [ ] task-8-tooltips.png

  **Commit**: YES (14/16)
  - Files: `gui/src/components/AdvancedNgxPanel.tsx` (new), `gui/src/components/SRSettings.tsx` (embed advanced panel), `gui/src/components/FGSettings.tsx` (enable-dynamic-resolution there), `gui/src/state/config-store.ts` (advanced slice), `gui/electron/cli-args.ts`
  - Pre-commit: `cd gui ; npm run typecheck && npm run lint`

- [x] 9. **Playwright spec suite — 5 new + 1 updated**

  **What to do**:
  - Add new spec files under `gui/tests/`:
    - **`exr-source-resolution.spec.ts`**: Verifies T3 + T4 — selecting an input dir triggers EXR header read; UI displays correct dims; mismatch with camera.json is ignored; missing file shows warning
    - **`resolution-aspect-lock.spec.ts`**: Verifies T6 — single-input W/H, bidirectional scale, orientation-aware presets, source-unknown fallback, floor mode
    - **`sr-fg-separation.spec.ts`**: Verifies T7 — three sections render, settings independent, BasicSettings absent
    - **`dlss-detect.spec.ts`**: Verifies T13 — banner received, StatusBar updates, error case handled (mock CLI exit-without-banner)
    - **`preview-error.spec.ts`**: Verifies T14 — FFmpeg present (success path), absent (banner shown), corrupted EXR (decode error), timeout (with mock)
  - Update existing **`real-processing.spec.ts`**: adapt to new UI (panel split, no path picker, source-resolution display, advanced panel exists). Smoke test: load fixture → set scale → start processing → wait for completion → verify output files exist.
  - Each spec uses Playwright's `test.beforeEach` to launch fresh Electron instance (or shared launcher utility). Use `electron.launch` from `playwright/test`.
  - Add helper `gui/tests/utils/launch-gui.ts` with shared launcher + common selectors (avoid duplication across specs)
  - Add fixtures `gui/tests/fixtures/`:
    - `standard_exr_1080p/` (5 frames, 1920×1080)
    - `standard_exr_portrait/` (5 frames, 1080×1920)
    - `mismatched_camera_json/` (EXR is 1920×1080 but camera.json says 4096×4096)
    - `corrupted_exr/` (random bytes named .exr)
    - `malformed_filenames/` (filename_.exr present)
  - Configure `playwright.config.ts` for proper Windows + Electron launch (likely already set up; verify)
  - All specs must produce evidence (screenshots, traces) saved to default Playwright output dir; CI can archive
  - Run full suite: `npx playwright test` — assert 100% pass

  **Must NOT do**:
  - Do NOT skip flaky-prone scenarios (timeout, mock subprocess) — write them robustly with explicit waits/retries instead
  - Do NOT use `page.waitForTimeout(N)` arbitrary sleeps — use `expect(...).toHaveText/toBeVisible` with built-in timeouts
  - Do NOT mock the entire IPC layer — these are integration tests; mock only the underlying CLI/FFmpeg when necessary
  - Do NOT duplicate Wave-1-3 fixtures (reference existing ones where possible)
  - Do NOT delete unrelated existing specs

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Test code spanning 6 files + fixtures + config; broad surface, requires breadth-first testing
  - **Skills**: `playwright` (project skill, if available — Playwright skill mentioned in skill list above; load if needed)

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T16)
  - **Parallel Group**: Wave 5
  - **Blocks**: F1-F4
  - **Blocked By**: T1, T2, T5, T6, T7, T8, T13, T14 (every behavioral task)

  **References**:

  **Pattern References**:
  - Existing specs in `gui/tests/` — copy structure, beforeEach, selectors
  - Playwright Electron docs

  **API/Type References**:
  - Playwright `test`, `expect`, `electron.launch`, `Page`, `ElectronApplication`
  - `playwright.config.ts` schema

  **External References**:
  - Playwright Electron testing guide: `https://playwright.dev/docs/api/class-electron`

  **WHY Each Reference Matters**:
  - Playwright's Electron API is distinct from web — proper launch + page-acquisition pattern is essential

  **Acceptance Criteria**:

  - [ ] 5 new spec files exist
  - [ ] 1 updated spec passes
  - [ ] All fixtures committed (small enough to not bloat repo — synthetic EXRs only)
  - [ ] `npx playwright test` exits 0
  - [ ] Test report shows ≥30 tests across new specs (rough lower bound — each spec has multiple scenarios)

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Full suite passes
    Tool: Bash (PowerShell)
    Preconditions: GUI built; node_modules installed; Playwright browsers installed (npx playwright install if first time)
    Steps:
      1. Run: cd gui ; npx playwright test --reporter=list 2>&1 | Tee-Object tmp/pw-output.txt
      2. Assert exit code 0
      3. Assert output contains "passed" lines for all expected specs
    Expected Result: 100% pass
    Evidence: .sisyphus/evidence/gui-rewrite/task-9-suite-output.txt + Playwright HTML report copy

  Scenario: Each new spec runnable in isolation
    Tool: Bash (PowerShell)
    Steps:
      1. For each new spec file: cd gui ; npx playwright test <spec-name> --reporter=list
      2. Assert each passes independently (no cross-spec dependencies)
    Expected Result: 6 isolated pass results
    Evidence: .sisyphus/evidence/gui-rewrite/task-9-isolated-runs.txt

  Scenario: Fixtures are valid EXR/JSON
    Tool: Bash (PowerShell)
    Steps:
      1. For each .exr in gui/tests/fixtures/: oiiotool --info ; assert exit 0 (or for corrupted_exr/, assert exit != 0 — that's the point)
      2. For each camera.json: ConvertFrom-Json ; assert no parse error
    Expected Result: Fixtures load as expected; corrupted is genuinely corrupted
    Evidence: .sisyphus/evidence/gui-rewrite/task-9-fixtures.txt
  ```

  **Evidence to Capture**:
  - [ ] task-9-suite-output.txt
  - [ ] task-9-isolated-runs.txt
  - [ ] task-9-fixtures.txt
  - [ ] Playwright HTML report (or copy of test-results dir)

  **Commit**: YES (15/16)
  - Files: `gui/tests/exr-source-resolution.spec.ts` (new), `gui/tests/resolution-aspect-lock.spec.ts` (new), `gui/tests/sr-fg-separation.spec.ts` (new), `gui/tests/dlss-detect.spec.ts` (new), `gui/tests/preview-error.spec.ts` (new), `gui/tests/real-processing.spec.ts` (updated), `gui/tests/utils/launch-gui.ts` (new), `gui/tests/fixtures/` (new), `gui/playwright.config.ts` (if needed)
  - Pre-commit: `cd gui ; npx playwright test`

- [x] 16. **Documentation rewrite — README + GUI/Blender/dev guides**

  **What to do**:
  - Update **`README.md`** "GUI Usage" section:
    - Remove references to "Set your executable path" (deleted in T10)
    - Update workflow: install → render → open GUI → process
    - Mention auto-detection of CLI binary
    - Mention settings persistence to `gui/dlss-compositor.json`
    - Add screenshot placeholder (existing TODO line)
  - Rewrite **`docs/gui_guide.md`**:
    - Sections: Overview, First Launch (auto-detection), Common Settings (input/output/etc.), SR Settings (scale, quality, presets, advanced), FG Settings (interpolation, camera-data, tonemap), Resolution Panel (aspect-lock, presets, manual scale), Preview & Status, Troubleshooting (DLSS detect failure, FFmpeg missing, source-resolution unknown, malformed files)
    - For each section, include: what it does, when to change defaults, link to relevant CLI flags in `usage_guide.md`
    - Power-user appendix: how to edit `gui/dlss-compositor.json` directly
  - Create **`docs/blender_export_guide.md`** (new):
    - Installation: install ZIP via Blender Preferences > Extensions > Install from Disk
    - Panel UI: Render Properties > DLSS Compositor — describe each control (compression, render scale, what passes are auto-set)
    - Standard EXR explanation: why we changed from MultiLayer; how to verify with RenderDoc
    - Channel naming convention: which file contains which channel; how MV is in sibling `.mv.exr`
    - Camera data export: when to click "Export Camera Data" (FG workflow); what camera.json contains
    - Render-time advice: bake textures, pre-denoise with OIDN, disable adaptive sampling for time-stable noise (existing best practice)
  - Create **`docs/development.md`** (T12 stub → enrich):
    - Prerequisites: Visual Studio 2022, CMake, Vulkan SDK, Node 20+, Blender 4.2+
    - Build C++ CLI: `cmake -S . -B build -G "Visual Studio 17 2022"` + `cmake --build build --config Release`
    - Build GUI dev: `cd gui ; npm install ; npm run dev`
    - Package GUI: `cd gui ; npm run package`
    - VSCode F5 workflow: 3 launch configs explained
    - Test commands: `ctest -C Release`, `cd gui ; npx playwright test`, headless Blender render scripts
    - Project structure: tree diagram of src/, gui/, blender/, tests/, docs/
    - Settings file: editing `gui/dlss-compositor.json` directly, schema reference
    - Adding new CLI flags: where to add in `cli_parser.cpp`, `config.h`, GUI advanced panel
    - Where to ask questions / file issues
  - Cross-references: README links to all 4 docs; each doc cross-links to relevant siblings
  - Verify all internal doc links resolve (no 404s)

  **Must NOT do**:
  - Do NOT add API documentation beyond README pointers (project doesn't currently maintain auto-generated API docs)
  - Do NOT use marketing language; technical reference tone throughout
  - Do NOT add screenshots speculatively (use TODO markers; T16 isn't responsible for capturing screenshots — leave for separate doc-polish pass)
  - Do NOT delete `docs/build_guide.md` if it exists (cross-link to it instead)
  - Do NOT translate docs to other languages

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Pure documentation work; benefits from technical-writing focus
  - **Skills**: none required

  **Parallelization**:
  - **Can Run In Parallel**: YES (with T9)
  - **Parallel Group**: Wave 5
  - **Blocks**: None
  - **Blocked By**: T1 (Blender format docs), T6 (Resolution UX docs), T7 (panel split docs), T10 (no path picker docs), T12 (F5 setup docs)

  **References**:

  **Pattern References**:
  - Existing `README.md` style and structure
  - Existing `docs/usage_guide.md` (referenced in README) — match tone

  **API/Type References**:
  - Markdown (CommonMark or GFM, whichever the repo uses)

  **External References**:
  - Standard technical-writing principles (clear, concise, examples-driven)

  **WHY Each Reference Matters**:
  - Maintaining tone consistency across docs builds user trust

  **Acceptance Criteria**:

  - [ ] `README.md` "GUI Usage" updated, no stale path-picker references
  - [ ] `docs/gui_guide.md` rewritten covering all new UX
  - [ ] `docs/blender_export_guide.md` exists with all sections
  - [ ] `docs/development.md` exists with all sections
  - [ ] All internal links resolve

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: All required doc files exist
    Tool: Bash (PowerShell)
    Steps:
      1. Test-Path README.md, docs/gui_guide.md, docs/blender_export_guide.md, docs/development.md
      2. Each must be > 1000 bytes (placeholder check)
    Expected Result: All exist with substantive content
    Evidence: .sisyphus/evidence/gui-rewrite/task-16-files.txt

  Scenario: No stale references
    Tool: Bash (PowerShell)
    Steps:
      1. Get-ChildItem README.md, docs/*.md | Select-String 'ExePathConfig|Set your executable path|OPEN_EXR_MULTILAYER'
      2. Assert no matches (these are removed concepts)
    Expected Result: Clean
    Evidence: .sisyphus/evidence/gui-rewrite/task-16-no-stale.txt

  Scenario: Internal links resolve
    Tool: Bash (PowerShell)
    Steps:
      1. For each markdown link [text](path) in README and docs/, parse path; if relative, Test-Path against repo root
      2. Assert all paths exist
    Expected Result: No broken links
    Evidence: .sisyphus/evidence/gui-rewrite/task-16-links.txt

  Scenario: F5 workflow doc matches launch.json
    Tool: Bash (PowerShell)
    Steps:
      1. Parse .vscode/launch.json → list configuration names
      2. Search docs/development.md → assert each name mentioned
    Expected Result: Doc consistent with config
    Evidence: .sisyphus/evidence/gui-rewrite/task-16-f5-doc.txt
  ```

  **Evidence to Capture**:
  - [ ] task-16-files.txt
  - [ ] task-16-no-stale.txt
  - [ ] task-16-links.txt
  - [ ] task-16-f5-doc.txt

  **Commit**: YES (16/16)
  - Files: `README.md`, `docs/gui_guide.md`, `docs/blender_export_guide.md` (new), `docs/development.md`
  - Pre-commit: link-check + grep for stale references

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
>
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback → fix → re-run → present again → wait for okay.

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read this plan end-to-end. For each "Must Have": verify implementation exists (open file, run binary, run GUI, parse output). For each "Must NOT Have": grep codebase for forbidden patterns:
  - `Get-ChildItem gui/src -Recurse -Filter *.tsx | Select-String 'ExePathConfig'` → expect zero matches
  - `Get-ChildItem gui/src -Recurse | Select-String 'render_width|render_height' | Where-Object { $_.Path -notmatch 'camera_data_loader|test' }` → expect zero matches in GUI
  - `Get-ChildItem gui/electron -Recurse | Select-String 'electron-store.*name'` → ensure no userData path used
  - `Get-ChildItem blender -Recurse -Filter *.py | Select-String 'OPEN_EXR_MULTILAYER'` → expect zero matches
  - Confirm `.gitignore` includes `gui/dlss-compositor.json`
  - Confirm `gui/dlss-compositor.example.json` exists
  Compare deliverables against handoff §4.2 plan blueprint.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [16/16] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `cd gui ; npm run typecheck` (zero errors), `cd gui ; npm run lint` (zero errors), `cmake --build build --config Release` (zero new warnings), `ctest -C Release --output-on-failure`. Review all changed files for: TypeScript `any` casts, `@ts-ignore`/`@ts-expect-error`, empty catches, `console.log` in production paths, commented-out code, unused imports. AI slop: wrapper components added without function ("just to organize"), generic prop names (`data`/`item`/`val`), JSDoc on every prop. React-specific: `useEffect` without deps array, state setters in render bodies, missing key props in lists.
  Output: `Build [PASS/FAIL] | Typecheck [PASS/FAIL] | Lint [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high` (+ `playwright` skill)
  Start from clean state: `cd gui ; rm -rf node_modules dist gui/dlss-compositor.json ; npm install ; npm run build`. Launch GUI from packaged build (or `npm run dev`). Execute every Playwright spec via `npx playwright test`. Then run integration scenarios:
  - Install Blender extension fresh, render 5 frames at 50% scale, verify standard EXR output opens in RenderDoc
  - Open GUI, verify auto-detected CLI path works, verify source resolution shows from EXR (not camera.json)
  - Set target via H only, verify W is computed and aspect locked
  - Toggle SR/FG panels, verify settings independent
  - Open Advanced, modify a flag, save, restart GUI, verify persistence
  - Trigger DLSS detect, verify either banner or explicit error (no silent hang for 30+ seconds)
  - Trigger FFmpeg preview with FFmpeg uninstalled, verify error message appears (not null)
  - Process malformed file (`filename_.exr`), verify warning + skip
  - F5 from VSCode, hit a breakpoint
  Save evidence to `.sisyphus/evidence/gui-rewrite/final-qa/`.
  Output: `Playwright [N/N pass] | Manual scenarios [N/N pass] | Integration [N/N] | Edge cases [N tested] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (`git diff master --name-only`, `git log --oneline master..HEAD`). Verify 1:1 — everything in spec was built (no missing), nothing beyond spec was built (no creep). Check "Must NOT do" compliance: no algorithm changes? no userData persistence added? no path picker re-introduced? no aspect-ratio toggle? Detect cross-task contamination: e.g., Task 6 (Resolution) shouldn't touch `SRSettings.tsx`. Flag unaccounted changes (random refactors, dependency upgrades).
  Output: `Tasks [16/16 compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

One commit per task (16 commits). Conventional commit format. Each includes `Refs: .sisyphus/plans/gui-rewrite.md#TaskN`. Order matches wave dependencies; tasks in same wave can be committed in any intra-wave order.

- **T1**: `feat(blender): switch exporter to standard EXR + panel UI for compression and scale` — `blender/dlss_compositor_aov/__init__.py`, `blender/aov_export_preset.py`. Pre-commit: blender headless render dry-run smoke test
- **T2**: `feat(exr): support both MultiLayer and standard channel naming` — `src/core/exr_reader.cpp`, `src/core/exr_reader.h`, `tests/test_exr_channel_aliases.cpp`
- **T3**: `feat(cli): --inspect-exr flag + GUI IPC channel for header read` — `src/cli/cli_parser.cpp`, `src/cli/config.h`, `src/inspection/exr_inspector.cpp`, `src/inspection/exr_inspector.h`, `gui/electron/main.ts`, `gui/electron/preload.ts`, `gui/electron/ipc-channels.ts`, `tests/test_exr_inspector.cpp`
- **T4**: `refactor(gui): drop camera.json render_width/height in favor of EXR header IPC` — `gui/electron/camera-detector.ts`, `gui/src/components/InputOutputPanel.tsx`
- **T5**: `fix(pipeline): skip files without trailing frame number with warning log` — `src/pipeline/sequence_processor.cpp`, `tests/test_sequence_filename_filter.cpp`
- **T6**: `feat(gui): aspect-locked ResolutionPanel with orientation-aware presets and bidirectional scale` — `gui/src/components/ResolutionPanel.tsx`, `gui/src/state/config-store.ts`, `gui/tests/__visual__/resolution-panel.spec.ts` (visual regression if present)
- **T7**: `refactor(gui): split BasicSettings into SRSettings, FGSettings, CommonSettings` — `gui/src/components/SRSettings.tsx` (new), `gui/src/components/FGSettings.tsx` (new), `gui/src/components/CommonSettings.tsx` (new), `gui/src/components/BasicSettings.tsx` (deleted), `gui/src/App.tsx`, `gui/src/state/config-store.ts`
- **T8**: `feat(gui): advanced NGX settings panel exposing Plan 1 flags` — `gui/src/components/SRSettings.tsx`, `gui/src/components/AdvancedNgxPanel.tsx` (new), `gui/src/state/config-store.ts`, `gui/electron/cli-args.ts`
- **T9**: `test(gui): playwright suite for new flows` — `gui/tests/exr-source-resolution.spec.ts`, `gui/tests/resolution-aspect-lock.spec.ts`, `gui/tests/sr-fg-separation.spec.ts`, `gui/tests/dlss-detect.spec.ts`, `gui/tests/preview-error.spec.ts`, `gui/tests/real-processing.spec.ts` (updated), `gui/tests/fixtures/` (new test EXRs)
- **T10**: `refactor(gui): drop ExePathConfig — auto-detect dev vs packaged exe` — `gui/src/components/ExePathConfig.tsx` (deleted), `gui/electron/main.ts`, `gui/electron/exe-locator.ts` (new), `gui/src/App.tsx`, `gui/src/state/config-store.ts`
- **T11**: `feat(gui): persist settings to gui/dlss-compositor.json` — `gui/electron/settings-store.ts`, `.gitignore`, `gui/dlss-compositor.example.json` (new)
- **T12**: `feat(dev): VSCode launch.json for F5 GUI dev/packaged + C++ debug` — `.vscode/launch.json` (new), `.vscode/tasks.json` (new), `docs/development.md` (new — or stub for T16)
- **T13**: `fix(gui): root-cause DLSS detection hang via stdio routing diagnosis` — `gui/electron/main.ts`, `gui/electron/process-manager.ts` (only banner-related lines), `src/cli/cli_parser.cpp` (banner emission), `src/dlss/ngx_wrapper.cpp` (banner consistency), `gui/src/components/StatusBar.tsx`
- **T14**: `fix(gui): structured FFmpeg preview errors with graceful UI degradation` — `gui/electron/preview-generator.ts`, `gui/src/components/ProcessingView.tsx` (error display only), `gui/electron/ipc-channels.ts`
- **T15**: `refactor(gui): simplify ProcessingView to preview-only (no duplicate progress)` — `gui/src/components/ProcessingView.tsx`, `gui/src/components/StatusBar.tsx` (if progress consolidated there), `gui/src/App.tsx` (layout)
- **T16**: `docs: rewrite GUI/Blender/dev guides for 2026 rework` — `README.md`, `docs/gui_guide.md`, `docs/blender_export_guide.md` (new), `docs/development.md`

---

## Success Criteria

### Verification Commands
```powershell
# Build (C++)
cmake --build build --config Release  # Expected: 0 errors, no new warnings

# Typecheck (TS)
cd gui ; npm run typecheck  # Expected: 0 errors

# Lint
cd gui ; npm run lint  # Expected: 0 errors

# Unit tests (C++)
ctest -C Release --output-on-failure  # Expected: 100% pass

# Playwright (GUI)
cd gui ; npx playwright test  # Expected: 100% pass on all specs

# Forbidden patterns absent
Get-ChildItem gui/src -Recurse -Filter *.tsx | Select-String 'ExePathConfig'  # Expected: 0 matches
Get-ChildItem gui/src -Recurse | Select-String 'render_width|render_height'  # Expected: 0 matches in GUI
Get-ChildItem blender -Recurse -Filter *.py | Select-String 'OPEN_EXR_MULTILAYER'  # Expected: 0 matches

# New affordances present
Test-Path gui/dlss-compositor.example.json  # Expected: True
Test-Path .vscode/launch.json  # Expected: True
Test-Path docs/blender_export_guide.md  # Expected: True
Test-Path docs/development.md  # Expected: True

# CLI flag works
dlss-compositor.exe --inspect-exr fixtures/sintel_short/frame_0001.exr | ConvertFrom-Json
# Expected: object with width, height, channels[]

# .gitignore covers user settings
Select-String -Path .gitignore -Pattern 'gui/dlss-compositor\.json'  # Expected: 1 match (the actual file, not .example)
```

### Final Checklist
- [x] All "Must Have" items verified present
- [x] All "Must NOT Have" items verified absent (grep + code review)
- [x] All 16 task QA scenarios produced evidence files
- [x] F1–F4 reviews all APPROVE
- [x] User explicitly okayed final state
