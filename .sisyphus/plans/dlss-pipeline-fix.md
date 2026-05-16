# DLSS Pipeline Fix — Per-Quality Preset, Advanced Settings, Camera Cuts

## TL;DR

> **Quick Summary**: Fix the per-quality preset bug (currently all 6 quality modes get the same preset value, defeating Phase 2 Task 3's design intent), expose every NGX advanced setting as a CLI flag for power users, add camera-cut detection so `InReset` fires on real cuts (not just sequence boundaries), and harden NGX init with DLL version logging + API version fallback (mirroring UE5.7 DLSS plugin v8.6.0 patterns).
>
> **Deliverables**:
> - Patched `src/dlss/ngx_wrapper.cpp` with per-quality preset logic + `DlssSRPreset::Auto` correctly mapped (DLAA/Quality/Balanced/UltraQuality=K, Performance=M, UltraPerformance=L)
> - New CLI flags: `--preset-dlaa`, `--preset-quality`, `--preset-balanced`, `--preset-performance`, `--preset-ultra-performance`, `--preset-ultra-quality`, `--feature-flag-hdr`/`--no-feature-flag-hdr`, `--feature-flag-mv-low-res`/`--no-...`, `--feature-flag-auto-exposure`/`--no-...`, `--enable-output-subrects`, `--exposure-scale`, `--frame-time-delta-ms`, `--dlss-sharpness`, `--enable-dynamic-resolution`/`--no-...`
> - Camera-cut detection in `sequence_processor.cpp` with `--camera-cut-translation-threshold`, `--camera-cut-rotation-threshold`, `--no-camera-cut-detection` flags
> - NGX init banner enriched with `nvngx_dlss.dll` file version (Win32 `GetFileVersionInfoW`) + API version fallback (retry with `NVSDK_NGX_Version_API_MACRO_BASE_LINE` on failure)
> - Unit tests for preset mapping, camera-cut detection, and integration fixture verification
>
> **Estimated Effort**: Medium-Large
> **Parallel Execution**: YES — 3 waves
> **Critical Path**: T1 (preset fix) → T2 (advanced flags) → T5 (verification)

---

## Context

### Original Request
User identified that the new `opus-rework` branch (atlas's implementation of Phase 0–4 plans) produces noticeably weaker DLSS-SR output than UE5.7 with DLSS 4.5 Plugin v8.6.0 on the same Preset M / 480p→4K test. Side-by-side investigation against the UE plugin source pinpointed two real defects in our pipeline plus two robustness gaps.

### Interview Summary

**Key Discussions** (captured in `.sisyphus/handoff/2026-rework-decisions.md`):
- **Per-quality preset implementation walked back**: Phase 2 Task 3 designed `DlssSRPreset::Auto` with per-quality differentiation, but the code in `src/dlss/ngx_wrapper.cpp:181-187` writes the same `presetValue` to all six `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_*` slots. UE5 plugin (`DLSSUpscaler.cpp:372-454`) has independent CVar lookups per quality mode — this is the core "secret sauce" gap.
- **Power-user mode is the policy**: User wants every NGX setting exposed as a CLI flag (and surfaced in GUI later via Plan 2). Defaults follow NVIDIA recommendations; users can override individually.
- **Camera-cut detection is intentionally simple**: Compare adjacent frame `matrix_world` translation/rotation deltas against thresholds. No ML, no shot detection — just geometry.
- **DLL version visibility matters**: When DLSS 5 ships, users will swap `nvngx_dlss.dll`. Logging the actual file version (not just NGX SDK header version) lets them confirm the swap took effect.
- **API version fallback is robustness, not bug fix**: UE plugin retries init with `version - 1` on failure. We adopt the same pattern in case driver mismatches cause first attempt to fail.

**Research Findings** (UE5.7 DLSS plugin v8.6.0 audit):
- `DLSSUpscaler.cpp:372-454`: Per-quality preset lookup via independent CVars
- `NGXRHI.cpp:563-578`: Feature creation flags identical to ours (`IsHDR | MVLowRes | AutoExposure`)
- `NGXRHI.cpp:580-614`: Create params structure matches ours
- `NGXVulkanRHI.cpp:71-102`: API version fallback pattern
- `VelocityCombinePass.cpp:80-173`: MV jitter correction — verified NOT needed for us (Blender Vector pass is jitter-free, confirmed in Phase 1 Task 3)
- `NGXRHI.cpp:675-697`: Preset parameter setup pattern

### Metis Review

This plan was generated from a comprehensive handoff document (`.sisyphus/handoff/2026-rework-decisions.md`) that already incorporates the equivalent of Metis-style gap analysis from the prior planning session. Key gaps already addressed in the handoff:

- **Gap**: How does `Auto` preset map to per-quality values? → **Resolved**: `DLAA/Quality/Balanced/UltraQuality=K, Performance=M, UltraPerformance=L`. This table is the authoritative specification of this plan, derived from the NVIDIA DLSS Programming Guide's general preset characteristics; the UE5 plugin source is referenced for the *pattern* of per-quality differentiation but does NOT itself define these specific values.
- **Gap**: How are conflicting flags handled (e.g., `--preset K` and `--preset-dlaa L`)? → **Resolved**: Per-quality flags override the global `--preset`. If only `--preset` is set, all quality modes use it (legacy behavior preserved). If only `--preset auto` is set, the per-quality table applies.
- **Gap**: Camera cut detection on first frame? → **Resolved**: First frame always sets `InReset = true` (existing behavior preserved); detection only kicks in from frame 1 onward.
- **Gap**: Does API version fallback risk silent feature loss? → **Resolved**: Log warning to NDJSON event when fallback path is taken, so user knows they're on `MACRO_BASE_LINE` API.

---

## Work Objectives

### Core Objective
Restore Phase 2 Task 3's design intent (per-quality preset mapping that actually differentiates), expose the full NGX surface for power users, and bring NGX init robustness in line with UE5 reference patterns — all without changing the public CLI defaults for users who don't set the new flags.

### Concrete Deliverables
1. `src/dlss/ngx_wrapper.cpp` patched: per-quality preset writes, with `DlssSRPreset::Auto` fully implemented
2. `src/cli/config.h` extended: new fields for per-quality presets, all NGX advanced settings, camera-cut thresholds
3. `src/cli/cli_parser.cpp` extended: new flags parsed and validated
4. `src/dlss/dlss_sr_processor.cpp` consumes new advanced settings (frame time delta override, sharpness, etc.)
5. `src/pipeline/sequence_processor.cpp` adds camera-cut detection in the per-frame loop
6. `src/core/camera_data_loader.cpp` exposes per-frame `matrix_world` accessor (if not already public)
7. NGX init banner emits new NDJSON event with DLL file version + API version path (primary or fallback)
8. Unit tests in `tests/` covering: preset mapping table, camera-cut threshold logic, advanced-flag parsing
9. Integration test: run a fixture sequence at 4 quality modes, parse NGX log, verify each mode received its preset
10. `docs/usage_guide.md` updated with new flags table

### Definition of Done
- [ ] `cmake --build build --config Release` completes without warnings introduced by this plan
- [ ] `ctest -C Release` passes all new tests
- [ ] `dlss-compositor.exe --help` lists every new flag with one-line description
- [ ] Running the canonical fixture (`tests/fixtures/sequence/`) at `--quality Quality --preset auto` yields a different `nvngx_dlss.log` preset trace than `--quality Performance --preset auto`
- [ ] Banner NDJSON event includes both `dlss_dll_version` and `ngx_api_version_used` fields
- [ ] Manual run with `--no-camera-cut-detection` produces identical output to the previous behavior (regression-free)

### Must Have
- Per-quality preset writes are independent (six distinct `NVSDK_NGX_Parameter_SetI` calls, six distinct values when `Auto`)
- All new flags default to current behavior (no behavior change for users who don't set them)
- DLL version reading is robust (handles missing DLL, missing version info, returns "unknown" string rather than crashing)
- API version fallback logs which version was used (no silent degradation)
- Camera-cut detection can be fully disabled via `--no-camera-cut-detection`

### Must NOT Have (Guardrails)
- ❌ Do NOT change DLSS-FG behavior — this plan is SR-only
- ❌ Do NOT touch `src/pipeline/frame_generator.cpp` or any FG path
- ❌ Do NOT modify Blender exporter (`blender/`) — Plan 2 owns that
- ❌ Do NOT touch GUI code (`gui/`) — Plan 2 owns GUI exposure
- ❌ Do NOT add new third-party dependencies (use Win32 `version.dll` for DLL version, already on Windows)
- ❌ Do NOT change the existing `--preset K` semantics — must remain backward-compatible
- ❌ Do NOT introduce shot-detection ML or feature-based scene change detection (geometry-only)
- ❌ Do NOT fix MV jitter (Phase 1 Task 3 already verified Blender Vector pass is jitter-free)
- ❌ Do NOT modify `IsHDR | MVLowRes | AutoExposure` defaults (already correct per UE5 audit)
- ❌ AI-slop: do NOT add wrapper utility classes for "abstracted preset management" — direct switch/map is fine
- ❌ AI-slop: do NOT add JSDoc/Doxygen comments on every line; only complex logic gets comments

---

## Verification Strategy (MANDATORY)

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.
> Acceptance criteria requiring "user manually tests/confirms" are FORBIDDEN.

### Test Decision
- **Infrastructure exists**: YES — CTest is wired (see `CMakeLists.txt`, existing tests in `tests/`)
- **Automated tests**: YES (Tests after implementation) — given C++ NGX surface, RED-GREEN-REFACTOR is hard for hardware-dependent tests; prefer "implement → unit test → integration test on real GPU fixture"
- **Framework**: CTest + Catch2 (existing convention)
- **Per-task QA**: Every task includes agent-executed scenarios run against the actual built binary on the canonical fixture

### QA Policy
Every task MUST include agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/dlss-pipeline-fix/task-{N}-{slug}.{ext}`.

- **CLI verification**: `Bash` runs `dlss-compositor.exe` with explicit flag combinations, captures stdout/stderr, asserts exit code + log content
- **NGX log inspection**: After fixture run, parse `nvngx_dlss.log` (NGX writes preset choices there), assert per-quality preset trace
- **Banner verification**: Run `--test-ngx --progress-format json`, parse NDJSON output, assert presence/values of new banner fields
- **Unit test execution**: `ctest -C Release -R <pattern>` — must show PASS for every new test name

---

## Execution Strategy

### Parallel Execution Waves

> Maximize throughput. Target: 5–8 tasks per wave. This plan has only 5 tasks total (per user spec) — waves are smaller but still parallel where possible.

```
Wave 1 (Start Immediately — independent foundation work):
├── Task 1: Per-quality preset fix (ngx_wrapper.cpp, config.h, cli_parser.cpp)        [deep]
├── Task 2: Expose all NGX advanced CLI flags (config.h, cli_parser.cpp, processors)  [unspecified-high]
└── Task 3: Camera cut detection (sequence_processor.cpp, config.h, cli_parser.cpp)   [deep]

Wave 2 (After Wave 1 — banner enrichment depends on advanced-flag CLI plumbing):
└── Task 4: NGX banner enrichment (DLL version + API version fallback)                [unspecified-high]

Wave 3 (After Waves 1–2 — integration verification needs everything in place):
└── Task 5: Integration verification + docs                                           [unspecified-high]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit                              [oracle]
├── Task F2: Code quality review                                [unspecified-high]
├── Task F3: Real manual QA on fixture                          [unspecified-high]
└── Task F4: Scope fidelity check                               [deep]
-> Present results -> Get explicit user okay

Critical Path: T1 → T4 → T5 → F1-F4 → user okay
Parallel Speedup: ~40% faster than sequential (3 of 5 tasks parallelizable in Wave 1)
Max Concurrent: 3 (Wave 1)
```

### Dependency Matrix

- **T1**: blocks → T4, T5  | blocked by → none
- **T2**: blocks → T5      | blocked by → none
- **T3**: blocks → T5      | blocked by → none
- **T4**: blocks → T5      | blocked by → T1, T2 (banner emits new advanced settings + preset choices)
- **T5**: blocks → F1-F4   | blocked by → T1, T2, T3, T4

### Agent Dispatch Summary

- **Wave 1**: 3 tasks — T1 → `deep`, T2 → `unspecified-high`, T3 → `deep`
- **Wave 2**: 1 task  — T4 → `unspecified-high`
- **Wave 3**: 1 task  — T5 → `unspecified-high`
- **FINAL**:  4 tasks — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

> Implementation + Test = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.

- [x] 1. **Fix per-quality preset writes (the core bug)**

  **What to do**:
  - Open `src/dlss/ngx_wrapper.cpp` lines 181–187. Currently every `NVSDK_NGX_Parameter_SetI(... Preset_*, presetValue)` writes the same `presetValue`. Replace with six independent writes sourced from new per-quality fields on `Config`.
  - Add to `src/cli/config.h` (in the `Config` struct, near existing `dlssPreset` field):
    ```cpp
    DlssSRPreset presetDLAA            = DlssSRPreset::Auto;
    DlssSRPreset presetQuality         = DlssSRPreset::Auto;
    DlssSRPreset presetBalanced        = DlssSRPreset::Auto;
    DlssSRPreset presetPerformance     = DlssSRPreset::Auto;
    DlssSRPreset presetUltraPerformance= DlssSRPreset::Auto;
    DlssSRPreset presetUltraQuality    = DlssSRPreset::Auto;
    ```
  - Implement `DlssSRPreset::Auto` resolution in a new free function `resolveAutoPreset(NVSDK_NGX_PerfQuality_Value)` returning the **explicit table mandated by this plan**, sourced directly from NVIDIA SDK header documentation (see References below — `DLSS/include/nvsdk_ngx_defs.h:82-85` documents these as the documented defaults): `DLAA → K`, `Quality → K`, `Balanced → K`, `UltraQuality → K` (NVIDIA documents K as the default for DLAA/Balanced/Quality; UltraQuality is not explicitly documented, K is chosen as a safe quality-preserving default), `Performance → M` (NVIDIA-documented default), `UltraPerformance → L` (NVIDIA-documented default). Implement these values exactly as listed.
  - Resolution priority for each slot: if per-quality field is non-Auto → use it. Else if global `dlssPreset` is non-Auto → use global. Else → use `resolveAutoPreset(quality)`.
  - Add CLI flags in `src/cli/cli_parser.cpp` (parse-loop): `--preset-dlaa <J|K|L|M|Auto>`, `--preset-quality`, `--preset-balanced`, `--preset-performance`, `--preset-ultra-performance`, `--preset-ultra-quality`. Reuse existing preset string→enum parser.
  - When user passes `--preset auto` (existing flag), set `config.dlssPreset = DlssSRPreset::Auto` and leave per-quality fields at `Auto` so all 6 slots resolve via the table.
  - Add unit test file `tests/test_preset_mapping.cpp` covering: (a) all-Auto resolution table matches the values mandated by this plan (DLAA/Quality/Balanced/UltraQuality=K, Performance=M, UltraPerformance=L); (b) global override works; (c) per-quality override beats global; (d) backward-compat — `--preset K` produces uniform K across all 6 slots.

  **Must NOT do**:
  - Do NOT introduce a `PresetResolver` class — direct switch in `resolveAutoPreset` is fine.
  - Do NOT change the meaning of `--preset K` (must still write K to all 6 slots when no per-quality flags set).
  - Do NOT touch FG path (`frame_generator.cpp`).
  - Do NOT add comments above every line of the resolution table.

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Multi-file refactor with backward-compat constraints, requires careful state-machine reasoning about flag precedence.
  - **Skills**: none required
  - **Skills Evaluated but Omitted**:
    - `git-master`: only one commit at end, standard workflow

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T2, T3)
  - **Blocks**: T4 (banner needs to know per-quality presets to log them), T5
  - **Blocked By**: None

  **References**:

  **Pattern References** (existing code to follow):
  - `src/dlss/ngx_wrapper.cpp:181-187` — current uniform-preset bug site (REPLACE this block)
  - `src/dlss/ngx_wrapper.cpp:175-179` — feature flags block (DO NOT touch — handoff verified correct)
  - `src/cli/cli_parser.cpp:206-209` — example of simple no-arg flag parsing (`--test-ngx`)

  **API/Type References**:
  - `src/cli/config.h` — `DlssSRPreset` enum (existing; add `Auto` if missing)
  - NVIDIA SDK header `nvsdk_ngx_defs.h` — `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_*` constants
  - NVIDIA SDK header — `NVSDK_NGX_PerfQuality_Value` enum (DLAA, MaxQuality, Balanced, MaxPerf, UltraPerformance, UltraQuality)

  **Test References**:
  - `tests/CMakeLists.txt` — existing test registration pattern (follow it)
  - Any existing `tests/test_*.cpp` — follow Catch2 TEST_CASE("...", "[tag]") / SECTION / REQUIRE / CHECK style (see existing tests/test_camera_data_loader.cpp for the canonical pattern)

  **External References**:
  - `D:\Github\dlss-compositor\DLSS\include\nvsdk_ngx_defs.h:82-85` — **AUTHORITATIVE source for the Auto table values**. Comments in this header document: K = "Default preset for DLAA/Balanced/Quality modes that is transformer based. Best image quality preset at a higher performance cost", L = "Default for Ultra Perf mode", M = "Default for Perf mode". The plan's Auto table follows these documented defaults verbatim (with K extended to UltraQuality, since header doesn't explicitly document a UltraQuality default).
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\DLSS\Private\DLSSUpscaler.cpp:372-454` — confirms the **pattern** of per-quality preset differentiation (independent CVar lookup per quality mode); the *specific values* this plan uses come from the NGX header above, not from this UE source.

  **WHY Each Reference Matters**:
  - `ngx_wrapper.cpp:181-187` — the exact 7 lines being replaced; executor needs to understand this is the bug site
  - `nvsdk_ngx_defs.h:82-85` — the **authoritative source** for Auto values; if NVIDIA later ships a new SDK with different documented defaults, that header is what to consult before changing the table
  - UE `DLSSUpscaler.cpp:372-454` — proves per-quality differentiation is an established pattern in production DLSS integrations (validates the *approach*); does NOT define our Auto values

  **Acceptance Criteria**:

  **Tests-after policy**:
  - [ ] `tests/test_preset_mapping.cpp` exists with ≥4 test cases (Auto table, global override, per-quality override, legacy `--preset K`)
  - [ ] `ctest -C Release -R PresetMapping` → 4/4 PASS

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Auto preset resolves to per-quality table
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded, fixture exists at tests/fixtures/sequence/
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/auto-q --scale 2 --quality Quality --preset auto --ngx-log-level verbose
      2. Read nvngx_dlss.log (NGX writes to working dir or %LOCALAPPDATA%\NVIDIA\NGX\logs\)
      3. grep for "Preset_Quality" hint write — assert hex/numeric value corresponds to K (NVSDK_NGX_DLSS_Hint_Render_Preset_K = 11)
      4. Run again with --quality Performance --preset auto
      5. grep for "Preset_Performance" hint write — assert value corresponds to M (= 13)
    Expected Result: Two runs produce different preset hints in NGX log per the Auto table
    Failure Indicators: Both runs log identical preset values OR no preset hints in log
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-1-auto-preset-trace.txt

  Scenario: Per-quality flag overrides global
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/override --scale 2 --quality Performance --preset K --preset-performance L --ngx-log-level verbose
      2. grep nvngx_dlss.log for the Preset_Performance hint write
      3. Assert value = L (= 12), NOT K
    Expected Result: --preset-performance L wins over --preset K
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-1-override-precedence.txt

  Scenario: Legacy --preset K still uniform (backward compat)
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/legacy --scale 2 --preset K --ngx-log-level verbose
      2. grep nvngx_dlss.log for ALL six Preset_* hint writes
      3. Assert all six log K (= 11)
    Expected Result: --preset K writes K to all 6 slots (no behavior change)
    Failure Indicators: Any slot logs a value other than K
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-1-legacy-uniform.txt
  ```

  **Evidence to Capture**:
  - [ ] task-1-auto-preset-trace.txt (NGX log excerpt for both Quality and Performance runs)
  - [ ] task-1-override-precedence.txt
  - [ ] task-1-legacy-uniform.txt
  - [ ] task-1-unit-test-output.txt (`ctest -R PresetMapping --verbose`)

  **Commit**: YES (1/5)
  - Message: `fix(dlss): per-quality preset writes (was uniform — Phase 2 Task 3 walkback)`
  - Files: `src/dlss/ngx_wrapper.cpp`, `src/cli/config.h`, `src/cli/cli_parser.cpp`, `tests/test_preset_mapping.cpp`, `tests/CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release && ctest -C Release -R PresetMapping`

- [x] 2. **Expose all NGX advanced settings as CLI flags**

  **What to do**:
  - Extend `src/cli/config.h` with new fields (group under `// NGX advanced (power-user)` comment block):
    ```cpp
    bool   enableOutputSubrects   = false;
    bool   featureFlagHDR         = true;
    bool   featureFlagMVLowRes    = true;
    bool   featureFlagAutoExposure= true;
    float  exposureScale          = 1.0f;
    float  frameTimeDeltaMsOverride = -1.0f;  // -1 = auto-calculate from --source-fps
    float  dlssSharpness          = 0.0f;     // NGX SR built-in sharpness; distinct from post-CAS
    bool   enableDynamicResolution= false;    // FG-only knob, kept here for completeness
    ```
  - Add CLI flag parsing in `src/cli/cli_parser.cpp` for each:
    - `--enable-output-subrects` / `--no-enable-output-subrects` (paired flags pattern)
    - `--feature-flag-hdr` / `--no-feature-flag-hdr`
    - `--feature-flag-mv-low-res` / `--no-feature-flag-mv-low-res`
    - `--feature-flag-auto-exposure` / `--no-feature-flag-auto-exposure`
    - `--exposure-scale <float>` (validate 0.0 < x ≤ 100.0)
    - `--frame-time-delta-ms <float>` (validate > 0.0; -1 sentinel for auto)
    - `--dlss-sharpness <float>` (validate 0.0 ≤ x ≤ 1.0)
    - `--enable-dynamic-resolution` / `--no-enable-dynamic-resolution`
  - Wire into `src/dlss/ngx_wrapper.cpp`:
    - Replace hardcoded `createParams.InEnableOutputSubrects = false` (line 179) with `config.enableOutputSubrects`
    - Replace hardcoded feature flags (lines 176-178) with conditional OR based on `featureFlag*` fields
  - Wire into `src/dlss/dlss_sr_processor.cpp`:
    - At line ~158 (where `InFrameTimeDeltaInMsec` is set): if `frameTimeDeltaMsOverride > 0` use it, else compute from source FPS as today
    - Pass `dlssSharpness` to NGX eval params (`InSharpness` field of `NVSDK_NGX_VK_DLSS_Eval_Params`)
    - Pass `exposureScale` to `InPreExposure` if AutoExposure flag is OFF; ignored if ON
  - Add `--help` text for every new flag (one line each, mention default value)
  - Add unit test `tests/test_cli_advanced_flags.cpp` covering each flag's parse + validation (negative values rejected, range bounds enforced)
  - Update `docs/usage_guide.md` with new section "Advanced NGX Settings (Power Users)" listing every flag with description, default, valid range, and one example use case

  **Must NOT do**:
  - Do NOT change ANY default — every new flag must default to the current hardcoded value (no behavior change for existing users)
  - Do NOT add new third-party dependencies for arg parsing — use existing parser style
  - Do NOT expose flags that don't have NGX backing (e.g., no `--magic-quality-boost`)
  - Do NOT validate `dlssSharpness` against quality mode (NGX accepts it for all modes)
  - Do NOT touch FG processor (`src/pipeline/frame_generator.cpp`)
  - Do NOT add a config file loader — CLI-only per Plan 1 scope

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Mostly mechanical CLI plumbing across 4 files, but breadth (8 flags × parse + validate + wire + test + doc) requires high diligence
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T1, T3)
  - **Blocks**: T4 (banner emits which flags are non-default), T5
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp` — search for an existing `--no-*` style flag pair as template (e.g., `--inverse-tonemap` / `--no-inverse-tonemap` if present)
  - `src/cli/cli_parser.cpp:206-209` — simple no-arg flag pattern
  - `src/dlss/ngx_wrapper.cpp:175-179` — feature-flag block (modify to be conditional)
  - `src/dlss/dlss_sr_processor.cpp:158` — `InFrameTimeDeltaInMsec` set point (Phase 1 already made this dynamic; verify and extend with override)

  **API/Type References**:
  - `nvsdk_ngx_defs.h` — `NVSDK_NGX_DLSS_Feature_Flags_*` enum
  - `nvsdk_ngx_params.h` — `NVSDK_NGX_VK_DLSS_Eval_Params` struct (fields: `InSharpness`, `InPreExposure`, etc.)

  **External References**:
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\NGXRHI\Private\NGXRHI.cpp:563-578` — full feature-flags list reference
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\NGXRHI\Private\NGXRHI.cpp:580-614` — create params reference (confirms field names + types)
  - NVIDIA DLSS Programming Guide — "Optional Eval Parameters" section

  **WHY Each Reference Matters**:
  - UE `NGXRHI.cpp:563-578` — confirms our flag set is complete (no UE-only flags we're missing)
  - `dlss_sr_processor.cpp:158` — Phase 1 already made frame time dynamic; this task only adds the manual override; executor must NOT undo Phase 1's work

  **Acceptance Criteria**:

  - [ ] `tests/test_cli_advanced_flags.cpp` covers all 8 flags (parse + validation)
  - [ ] `ctest -C Release -R CliAdvancedFlags` → all PASS
  - [ ] `dlss-compositor.exe --help` shows all 8 new flags
  - [ ] `docs/usage_guide.md` has "Advanced NGX Settings" section

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: All new flags discoverable in --help
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded
    Steps:
      1. Run: dlss-compositor.exe --help > tmp/help.txt
      2. For each flag in [enable-output-subrects, feature-flag-hdr, feature-flag-mv-low-res, feature-flag-auto-exposure, exposure-scale, frame-time-delta-ms, dlss-sharpness, enable-dynamic-resolution]: Select-String -Path tmp/help.txt -Pattern "--$flag"
    Expected Result: All 8 flags present in help output
    Failure Indicators: Any flag missing
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-2-help-coverage.txt

  Scenario: Validation rejects out-of-range values
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/x --dlss-sharpness 1.5 ; assert exit code != 0
      2. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/x --dlss-sharpness -0.1 ; assert exit code != 0
      3. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/x --exposure-scale 0 ; assert exit code != 0
      4. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/x --dlss-sharpness 0.5 ; assert exit code == 0 (valid)
    Expected Result: 3 validation rejections, 1 acceptance, with clear stderr error messages on rejections
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-2-validation.txt

  Scenario: Backward compat — no flag changes mean no behavior change
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; baseline output from pre-plan binary saved at tmp/baseline/
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/post --scale 2 --quality Quality --preset K
      2. For each output frame: fc /b tmp/baseline/frame_0001.exr tmp/post/frame_0001.exr (binary diff)
    Expected Result: Identical bytes (or NGX-determinism allowed delta documented in fixture README)
    Failure Indicators: Any byte difference in output frames not explained by NGX non-determinism
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-2-backward-compat.txt
  ```

  **Evidence to Capture**:
  - [ ] task-2-help-coverage.txt
  - [ ] task-2-validation.txt
  - [ ] task-2-backward-compat.txt
  - [ ] task-2-unit-test-output.txt

  **Commit**: YES (2/5)
  - Message: `feat(cli): expose all NGX advanced settings as flags`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`, `src/dlss/ngx_wrapper.cpp`, `src/dlss/dlss_sr_processor.cpp`, `tests/test_cli_advanced_flags.cpp`, `tests/CMakeLists.txt`, `docs/usage_guide.md`
  - Pre-commit: `ctest -C Release -R CliAdvancedFlags`

- [x] 3. **Camera cut detection (translation/rotation thresholds)**

  **What to do**:
  - **Math approach**: Use the project's existing math (no new dependencies). The `CameraDataLoader` already extracts per-frame `position[3]` (translation column of `matrix_world`) and `forward[3]` (Z-basis vector) — see `src/core/camera_data_loader.cpp:119-122`. Reuse these instead of pulling in GLM.
    - Translation delta: euclidean distance between `prev.position` and `curr.position` (3-component vector).
    - Rotation delta: angle between forward vectors via `acos(clamp(dot(prev.forward, curr.forward), -1.0, 1.0))` converted to degrees. This catches yaw + pitch changes (the dominant cut indicators); pure roll without translation is intentionally not detected as a cut (acceptable per "must not have ML/feature detection" guardrail).
    - If executor needs a true 3-DOF rotation delta, extend `MatrixUtil` (in `src/core/camera_data_loader.cpp:16-99`) with a helper `rotationAngleDegrees(const float (&R_prev)[4][4], const float (&R_curr)[4][4])` using the trace formula: `cos(theta) = (trace(R_curr * R_prev^T) - 1) / 2`. Prefer the simpler `acos(dot(forward, forward))` approach unless QA reveals it misses real cuts.
  - In `src/pipeline/sequence_processor.cpp` per-frame loop (around the spot where `InReset` is currently set on frame 0 / sequence boundaries), add detection logic. Pseudocode (use raw C-style math, NOT GLM):
    ```cpp
    bool isCameraCut = false;
    if (frameIndex > 0 && config.cameraCutDetectionEnabled && cameraData.has_value()) {
        const auto& prev = cameraData->getFrameByIndex(frameIndex - 1);
        const auto& curr = cameraData->getFrameByIndex(frameIndex);
        // Translation delta — simple euclidean distance on extracted position[3]
        float dx = curr.position[0] - prev.position[0];
        float dy = curr.position[1] - prev.position[1];
        float dz = curr.position[2] - prev.position[2];
        const float translationDelta = std::sqrt(dx*dx + dy*dy + dz*dz);
        // Rotation delta — angle between forward vectors
        float dot = prev.forward[0]*curr.forward[0]
                  + prev.forward[1]*curr.forward[1]
                  + prev.forward[2]*curr.forward[2];
        if (dot >  1.0f) dot =  1.0f;
        if (dot < -1.0f) dot = -1.0f;
        const float rotationDeltaDeg = std::acos(dot) * (180.0f / 3.14159265358979323846f);
        if (translationDelta > config.cameraCutTranslationThreshold ||
            rotationDeltaDeg > config.cameraCutRotationThreshold) {
            isCameraCut = true;
        }
    }
    if (isCameraCut) { evalParams.reset = 1; /* emit NDJSON event */ }
    ```
  - Add to `src/cli/config.h`:
    ```cpp
    bool  cameraCutDetectionEnabled    = true;   // default ON; --no-camera-cut-detection disables
    float cameraCutTranslationThreshold= 5.0f;   // world units
    float cameraCutRotationThreshold   = 30.0f;  // degrees
    ```
  - Add CLI flags: `--camera-cut-translation-threshold <float>`, `--camera-cut-rotation-threshold <float>`, `--no-camera-cut-detection`
  - Camera frame access: `CameraDataLoader` already loads frames into a sequence; verify (or add) a public method `getFrameByIndex(int)` that returns the per-frame struct containing `position[3]` and `forward[3]`. If accessor doesn't exist, add it next to existing public API in `src/core/camera_data_loader.h`.
  - Emit NDJSON progress event when cut detected: `{"type":"camera_cut","frame":N,"translation_delta":X,"rotation_delta_deg":Y}` — uses existing NDJSON emission helper
  - Unit test `tests/test_camera_cut_detection.cpp`:
    - Synthetic sequence: identity transform for frames 0–4, large jump (translation > 5.0) at frame 5 → assert frame 5 detected as cut
    - Pure rotation: 31° yaw between frames → assert detected
    - Sub-threshold: 4.9 units translation, 29° rotation → assert NOT detected
    - With detection disabled: above same large jumps → assert NOT detected
    - First frame (frameIndex=0): assert NEVER detected as cut (always falls through to existing first-frame reset)

  **Must NOT do**:
  - Do NOT add GLM, Eigen, or any other math library — this project uses raw C arrays + the in-source `MatrixUtil` namespace (`src/core/camera_data_loader.cpp:16-99`); follow that convention
  - Do NOT add ML-based or feature-based shot detection
  - Do NOT change `InReset` behavior on frame 0 or sequence-boundary cases (preserve existing logic; add to it, not replace)
  - Do NOT make camera-cut detection mandatory — `--no-camera-cut-detection` must fully disable
  - Do NOT touch FG path
  - Do NOT modify `camera.json` schema (consume existing `matrix_world` field as-is)
  - Do NOT add a "smart adaptive threshold" — fixed thresholds with user overrides only

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Geometry math (quaternion angle from rotation matrices) requires care; off-by-one in frame indexing has cascading effects on temporal history reset
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T1, T2)
  - **Blocks**: T5
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp` — search for current `InReset` / `evalParams.reset = 1` site (existing first-frame and sequence-boundary handling)
  - `src/core/camera_data_loader.cpp:16-99` — `MatrixUtil` namespace (existing math helpers — copy, identity, inverse); extend here if a true 3-DOF rotation delta is needed beyond `acos(dot(forward, forward))`
  - `src/core/camera_data_loader.cpp:119-122` — pre-extracted `position[3]` and `forward[3]` per frame (consume these directly)
  - `src/core/camera_data_loader.cpp:166-176` — `render_width`/`render_height` parse pattern (for adding new accessor or new field)

  **API/Type References**:
  - `src/core/camera_data_loader.h` — `CameraDataLoader` class public API; verify or add `getFrameByIndex(int)` returning the per-frame struct with `position[3]`, `forward[3]`, `matrix_world[4][4]`
  - `<cmath>` — `std::sqrt`, `std::acos` (already pulled in by camera_data_loader.cpp)
  - `nvsdk_ngx_params.h` — `NVSDK_NGX_VK_DLSS_Eval_Params::InReset` field

  **Test References**:
  - Existing tests in `tests/` — copy CMake registration pattern

  **External References**:
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\DLSS\Private\DLSSUpscaler.cpp` — search for `bCameraCut` references — UE uses scene cut state from engine; we synthesize from camera matrix
  - Trace-based rotation angle formula (well-known): `theta = acos((trace(R) - 1) / 2)` for a 3x3 rotation matrix R; only needed if forward-vector approach proves insufficient

  **WHY Each Reference Matters**:
  - UE source confirms `InReset = true` on camera cut is the correct NGX usage; we just synthesize the trigger differently (engine doesn't tell us, geometry does)
  - `position[3]` / `forward[3]` already extracted means zero new code in `camera_data_loader` for the simple approach — keeps the change surface minimal
  - `MatrixUtil` namespace is the project's established math home — any new helpers (e.g., trace-based rotation angle) belong here, not in a new file

  **Acceptance Criteria**:

  - [ ] `tests/test_camera_cut_detection.cpp` passes 5 scenarios
  - [ ] `ctest -C Release -R CameraCut` → all PASS
  - [ ] NDJSON emission verified in QA scenario

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Camera cut detected on synthetic jump
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; **CREATE fixture as part of this task** at tests/fixtures/cut_test_synthetic/ with this structure:
      - Copy frame_0001.exr through frame_0005.exr from tests/fixtures/sequence/ (filenames preserved — keeps existing 1-5 numbering)
      - Author a new camera.json describing 5 frames keyed by frame number "1" through "5" (or whatever schema CameraDataLoader expects — verify by reading src/core/camera_data_loader.cpp:188-210 first):
        - Frames 1-4: matrix_world = identity (4x4 with diagonal 1s, translation column [0,0,0,1])
        - Frame 5: matrix_world = identity rotation, but translation column [10, 0, 0, 1] (10-unit jump in X relative to frame 4)
      - Add tests/fixtures/cut_test_synthetic/README.md documenting: "Synthetic camera-cut fixture: frames 1-4 stationary, frame 5 jumps 10 units in X. Used by Plan dlss-pipeline-fix Task 3."
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/cut_test_synthetic --output-dir tmp/cut --scale 2 --camera-data tests/fixtures/cut_test_synthetic/camera.json --progress-format json 2> tmp/cut.ndjson
      2. Select-String -Path tmp/cut.ndjson -Pattern '"type":"camera_cut".*"frame":5'
      3. Select-String -Path tmp/cut.ndjson -Pattern '"type":"camera_cut".*"frame":[1-4]' ; assert no matches (frames 1-4 must NOT trigger cut)
    Expected Result: Exactly one camera_cut event for frame 5 with translation_delta>=10
    Failure Indicators: No camera_cut event OR cut on a frame other than 5 OR multiple cuts
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-3-cut-detected.ndjson

  Scenario: --no-camera-cut-detection disables cuts entirely
    Tool: Bash (PowerShell)
    Preconditions: Same fixture
    Steps:
      1. Run: dlss-compositor.exe --input-dir tests/fixtures/cut_test_synthetic --output-dir tmp/no-cut --scale 2 --camera-data tests/fixtures/cut_test_synthetic/camera.json --no-camera-cut-detection --progress-format json 2> tmp/no-cut.ndjson
      2. Select-String -Path tmp/no-cut.ndjson -Pattern '"type":"camera_cut"' ; assert no matches
    Expected Result: Zero camera_cut events
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-3-disabled.ndjson

  Scenario: Threshold tuning works
    Tool: Bash (PowerShell)
    Preconditions: Same fixture (frame 5 jumps by 10 units)
    Steps:
      1. Run with default thresholds (5.0): expect cut detected
      2. Run with --camera-cut-translation-threshold 20: expect NO cut detected (10 < 20)
    Expected Result: Threshold above the jump magnitude suppresses detection
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-3-threshold-tuning.txt
  ```

  **Evidence to Capture**:
  - [ ] task-3-cut-detected.ndjson
  - [ ] task-3-disabled.ndjson
  - [ ] task-3-threshold-tuning.txt
  - [ ] task-3-unit-test-output.txt
  - [ ] fixture README documenting the synthetic camera.json structure

  **Commit**: YES (3/5)
  - Message: `feat(pipeline): camera cut detection via translation/rotation thresholds`
  - Files: `src/pipeline/sequence_processor.cpp`, `src/core/camera_data_loader.cpp` (if new accessor), `src/core/camera_data_loader.h`, `src/cli/config.h`, `src/cli/cli_parser.cpp`, `tests/test_camera_cut_detection.cpp`, `tests/CMakeLists.txt`, `tests/fixtures/cut_test_synthetic/` (synthetic test fixture)
  - Pre-commit: `ctest -C Release -R CameraCut`

- [x] 4. **NGX banner enrichment: DLL version + API version fallback**

  **What to do**:
  - In `src/dlss/ngx_wrapper.cpp` (or a new helper `src/dlss/ngx_dll_info.cpp`), add a free function `std::string getNgxDllFileVersion()`:
    - Locate `nvngx_dlss.dll`: try `LoadLibraryW(L"nvngx_dlss.dll")` then `GetModuleFileNameW`. If not loaded, walk standard NGX paths (`%LOCALAPPDATA%\NVIDIA\NGX\models\dlss\` etc.) — but simplest is: only attempt after NGX init succeeds (then DLL is mapped).
    - Use Win32 `GetFileVersionInfoSizeW` → `GetFileVersionInfoW` → `VerQueryValueW(L"\\")` to extract `VS_FIXEDFILEINFO`, format as `"<major>.<minor>.<build>.<revision>"`.
    - On any failure return `"unknown"`.
    - Link against `version.lib` (add to CMake target `target_link_libraries(... version)`).
  - Modify NGX init at `src/dlss/ngx_wrapper.cpp:106` to wrap in version-fallback try. **Note**: this repo's bundled `DLSS/include/nvsdk_ngx_defs.h:56,92` defines only `NVSDK_NGX_VERSION_API_MACRO` (currently `0x0000015` = NGX 1.5.0) and `NVSDK_NGX_Version_API` (the enum value backed by it). There is **no pre-defined baseline/fallback symbol** in the header. We define our own fallback as `MACRO - 1` (= NGX 1.4.0), mirroring the UE5 plugin's `version - 1` retry pattern (`NGXVulkanRHI.cpp:71-102`):
    ```cpp
    // Define near top of ngx_wrapper.cpp (in anonymous namespace or as constexpr)
    constexpr uint32_t kNgxApiVersionPrimary  = NVSDK_NGX_VERSION_API_MACRO;       // e.g. 0x15
    constexpr uint32_t kNgxApiVersionFallback = NVSDK_NGX_VERSION_API_MACRO - 1;   // e.g. 0x14

    // ... in init function:
    NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion, kNgxWorkDir,
        instance, physicalDevice, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        &featureInfo, static_cast<NVSDK_NGX_Version>(kNgxApiVersionPrimary));
    bool usedFallback = false;
    if (NVSDK_NGX_FAILED(initResult)) {
        // Fallback: retry with version - 1 (mirrors UE5 plugin pattern)
        initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion, kNgxWorkDir,
            instance, physicalDevice, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
            &featureInfo, static_cast<NVSDK_NGX_Version>(kNgxApiVersionFallback));
        if (!NVSDK_NGX_FAILED(initResult)) usedFallback = true;
    }
    if (NVSDK_NGX_FAILED(initResult)) {
        errorMsg = "NGX init failed (both primary " + toHex(kNgxApiVersionPrimary)
                 + " and fallback " + toHex(kNgxApiVersionFallback) + " versions): "
                 + ngxResultToString(initResult);
        return false;
    }
    m_ngxApiVersionUsed = usedFallback ? kNgxApiVersionFallback : kNgxApiVersionPrimary;
    m_ngxApiVersionUsedLabel = usedFallback ? "fallback" : "primary";  // for banner JSON
    ```
  - Modify the banner emission point (search for where `--test-ngx` outputs banner JSON) to include new fields:
    ```json
    {
      "type":"banner",
      "ngx_sdk_version":"1.5.0",
      "ngx_api_version_used":"0x15",          // hex of actual version used
      "ngx_api_version_path":"primary",       // "primary" or "fallback"
      "dlss_dll_version":"3.7.20.0",
      "driver_version":"...",
      "gpu":"..."
    }
    ```
    If fallback path was taken, additionally emit a separate warning event:
    ```json
    {"type":"warning","code":"ngx_api_fallback","detail":"Primary NGX API version 0x15 failed; succeeded with fallback 0x14"}
    ```
  - Add unit test `tests/test_ngx_banner.cpp` (where possible without real GPU): mock `getNgxDllFileVersion` to return known string, assert banner JSON contains it. Real GPU integration test is in T5.

  **Must NOT do**:
  - Do NOT make `version.lib` a runtime dependency (it's a Windows system DLL — already present)
  - Do NOT remove the original primary version attempt — fallback is secondary, not replacement
  - Do NOT crash if DLL version can't be read — return `"unknown"` and continue
  - Do NOT log the fallback as an error — it's a warning-level NDJSON event with `"code":"ngx_api_fallback"`
  - Do NOT use the symbol name `NVSDK_NGX_Version_API_MACRO_BASE_LINE` or `NVSDK_NGX_Version_API_MACRO_BASE_LINE` — it doesn't exist in the bundled SDK header (this repo's `DLSS/include/nvsdk_ngx_defs.h`); define our own `kNgxApiVersionFallback = NVSDK_NGX_VERSION_API_MACRO - 1` instead
  - Do NOT touch FG init paths

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Win32 API + NGX init plumbing — moderate complexity, cross-cutting (linker config, init path, banner emission)
  - **Skills**: none

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (alone — depends on T1+T2 to know what to log)
  - **Blocks**: T5
  - **Blocked By**: T1 (banner needs to log resolved per-quality presets), T2 (banner needs to log non-default advanced settings)

  **References**:

  **Pattern References**:
  - `src/dlss/ngx_wrapper.cpp:106` — current init call (wrap with fallback)
  - Banner emission point — grep for existing `"type":"banner"` or `--test-ngx` output code

  **API/Type References**:
  - Win32 `version.h` — `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerQueryValueW`, `VS_FIXEDFILEINFO`
  - This repo's NGX SDK header `D:\Github\dlss-compositor\DLSS\include\nvsdk_ngx_defs.h:56,92`:
    - `NVSDK_NGX_VERSION_API_MACRO` (line 56) is the only version macro defined; current value `0x0000015` (1.5.0)
    - `NVSDK_NGX_Version_API` (line 92) is an enum value backed by the above macro
    - **No baseline/fallback symbol exists** — define `kNgxApiVersionFallback = NVSDK_NGX_VERSION_API_MACRO - 1` ourselves, then cast to `NVSDK_NGX_Version` when passing to `NVSDK_NGX_VULKAN_Init_with_ProjectID`

  **External References**:
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\NGXVulkanRHI\Private\NGXVulkanRHI.cpp:71-102` — UE's API version fallback pattern (the canonical reference)
  - Microsoft docs: `https://learn.microsoft.com/en-us/windows/win32/api/winver/nf-winver-getfileversioninfow`

  **WHY Each Reference Matters**:
  - UE `NGXVulkanRHI.cpp:71-102` — confirms fallback is industry pattern, not a workaround
  - MS docs — for executor unfamiliar with Win32 file-version API; the 3-call dance (size → buf → query) is non-obvious

  **Acceptance Criteria**:

  - [ ] `tests/test_ngx_banner.cpp` covers DLL version mock + banner field presence
  - [ ] `ctest -C Release -R NgxBanner` → PASS
  - [ ] CMake links `version.lib` on the target

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Banner includes DLL version on real GPU
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; NVIDIA RTX GPU present; nvngx_dlss.dll present in NGX search path
    Steps:
      1. Run: dlss-compositor.exe --test-ngx --progress-format json > tmp/banner.ndjson 2>&1
      2. Select-String -Path tmp/banner.ndjson -Pattern '"dlss_dll_version":"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"'
      3. Select-String -Path tmp/banner.ndjson -Pattern '"ngx_api_version_path":"(primary|fallback)"'
      4. Select-String -Path tmp/banner.ndjson -Pattern '"ngx_api_version_used":"0x1[45]"'
    Expected Result: All three fields present; dll_version is non-"unknown" on standard install; primary path used in normal case
    Failure Indicators: Any field missing OR dll_version is "unknown" (means DLL detection broke)
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-4-banner-real-gpu.ndjson

  Scenario: Banner gracefully handles missing DLL (renamed)
    Tool: Bash (PowerShell)
    Preconditions: Temporarily rename nvngx_dlss.dll in standard path (or block its load via PATH manipulation)
    Steps:
      1. Run: dlss-compositor.exe --test-ngx --progress-format json > tmp/banner-nodll.ndjson 2>&1
      2. Verify exit code != 0 (NGX init should fail) OR banner contains "dlss_dll_version":"unknown"
    Expected Result: Either clean failure with error event, or "unknown" version string — no crash
    Failure Indicators: Process crashes with access violation OR hangs
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-4-banner-no-dll.ndjson

  Scenario: API version fallback warning emitted (synthetic)
    Tool: Catch2 unit test (mock NGX init function pointer to fail first call, succeed second)
    Preconditions: tests/test_ngx_banner.cpp written using Catch2 conventions (TEST_CASE / SECTION / REQUIRE / CHECK)
    Steps:
      1. In test: stub the NGX init wrapper to return failure when called with kNgxApiVersionPrimary
      2. Stub returns success when called with kNgxApiVersionFallback
      3. Invoke ngx_wrapper init; assert m_ngxApiVersionUsedLabel == "fallback"
      4. Capture banner emission; assert event includes "ngx_api_version_path":"fallback"
      5. Assert separate warning event with "code":"ngx_api_fallback" was emitted
    Expected Result: Fallback path exercised, warning logged
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-4-fallback-mock.txt
  ```

  **Evidence to Capture**:
  - [ ] task-4-banner-real-gpu.ndjson
  - [ ] task-4-banner-no-dll.ndjson
  - [ ] task-4-fallback-mock.txt
  - [ ] task-4-cmake-version-lib.txt (verify `version.lib` in link line via `Get-Content build/CMakeFiles/dlss-compositor.dir/link.txt`)

  **Commit**: YES (4/5)
  - Message: `feat(dlss): banner emits DLL version + API version fallback`
  - Files: `src/dlss/ngx_wrapper.cpp`, `src/dlss/ngx_dll_info.cpp` (new), `src/dlss/ngx_dll_info.h` (new), `src/cli/cli_parser.cpp` (banner output point), `CMakeLists.txt` (link version.lib), `tests/test_ngx_banner.cpp`, `tests/CMakeLists.txt`
  - Pre-commit: `dlss-compositor.exe --test-ngx --progress-format json | findstr dlss_dll_version`

- [x] 5. **Integration verification + documentation**

  **What to do**:
  - Add integration test `tests/test_integration_preset_trace.cpp` that runs the binary as a subprocess. Reuse the existing fixture `tests/fixtures/sequence/` (5 frames, suitable size). For each of 4 quality modes (`Quality`, `Balanced`, `Performance`, `UltraPerformance`) with `--preset auto`:
    - Spawn process with `--ngx-log-level verbose`
    - Wait for completion
    - Open `nvngx_dlss.log` (path is well-known per NGX docs; use `%LOCALAPPDATA%\NVIDIA\NGX\logs\` or working dir)
    - grep for `Hint_Render_Preset_Quality` (or matching slot) write line
    - Assert the value matches the Auto table (Quality/Balanced=K=5, Performance=M=7, UltraPerformance=L=6)
  - Mark this test as a "GPU-required" tag in CTest so it's skipped on CI runners without RTX hardware. Use CTest fixture/label feature: `set_tests_properties(IntegrationPresetTrace PROPERTIES LABELS "gpu_required")`.
  - Update `docs/usage_guide.md` final pass:
    - Add "DLSS Preset Selection" subsection explaining Auto + per-quality overrides
    - Add example invocations showcasing new flags
    - Cross-link to NVIDIA DLSS Programming Guide
  - Create new doc `docs/dlss_advanced.md`:
    - Explain every advanced flag from T2 (purpose, valid range, when to use, NVIDIA recommendation)
    - Document camera-cut detection: how it works, threshold tuning, when to disable
    - Document banner fields including `dlss_dll_version` and `ngx_api_version_used`
    - Document API version fallback behavior
    - Reference UE5 plugin source for power users wanting to dive deeper
  - Re-run full `ctest -C Release` to confirm no regressions across plan

  **Must NOT do**:
  - Do NOT add new features beyond verification + docs
  - Do NOT mark integration test as required on CI runners without GPU (label-only)
  - Do NOT duplicate flag descriptions between `usage_guide.md` and `dlss_advanced.md` — usage guide gets summary table, advanced doc gets full prose
  - Do NOT auto-generate docs from code — manual prose is required for "when to use" guidance
  - Do NOT commit the integration test's `nvngx_dlss.log` artifact

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Subprocess orchestration in test code + docs writing across two files; benefits from broad context handling
  - **Skills**: none required (writing skill not needed for technical reference docs)

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (alone — needs all prior tasks merged)
  - **Blocks**: F1-F4 (final review wave)
  - **Blocked By**: T1, T2, T3, T4

  **References**:

  **Pattern References**:
  - Existing integration tests in `tests/` — copy subprocess invocation pattern (likely uses `std::system` or `popen`)
  - `docs/usage_guide.md` — current structure (sections, code-block style)

  **API/Type References**:
  - CTest `set_tests_properties` with `LABELS` — CMake docs: `https://cmake.org/cmake/help/latest/prop_test/LABELS.html`
  - NGX log file location — NVIDIA NGX SDK README

  **External References**:
  - NGX SDK Programming Guide PDF — for docs cross-references
  - `D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS\Source\DLSS\Private\DLSSUpscaler.cpp:372-454` — referenced from advanced docs as "for power users"

  **WHY Each Reference Matters**:
  - CTest LABELS — lets CI runners filter `ctest -L gpu_required` or `ctest -LE gpu_required` cleanly

  **Acceptance Criteria**:

  - [ ] `tests/test_integration_preset_trace.cpp` runs against real fixture, asserts per-quality preset values
  - [ ] `ctest -C Release -L gpu_required` → PASS on machine with RTX
  - [ ] `docs/usage_guide.md` has "DLSS Preset Selection" subsection
  - [ ] `docs/dlss_advanced.md` exists with all advanced flags documented
  - [ ] Full `ctest -C Release` (excluding GPU label) passes — no regressions

  **QA Scenarios** (MANDATORY):

  ```
  Scenario: Integration test confirms per-quality preset trace
    Tool: Bash (PowerShell)
    Preconditions: Build succeeded; RTX GPU present; fixture is the existing `tests/fixtures/sequence/` (no new fixture needed)
    Steps:
      1. Run: ctest -C Release -L gpu_required --output-on-failure -R IntegrationPresetTrace
      2. Capture full output
    Expected Result: Test passes — log shows: Quality run wrote K (5) to Preset_Quality slot, Balanced wrote K to Preset_Balanced, Performance wrote M (7) to Preset_Performance, UltraPerformance wrote L (6) to Preset_UltraPerformance
    Failure Indicators: Any quality mode logs unexpected preset value
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-5-integration-trace.txt

  Scenario: Documentation completeness
    Tool: Bash (PowerShell)
    Preconditions: T1-T4 merged; doc files written
    Steps:
      1. For each new flag from T1+T2+T3 (~17 flags): Select-String -Path docs/dlss_advanced.md -Pattern "--$flag"
      2. Verify all 17 found
      3. Select-String -Path docs/usage_guide.md -Pattern "DLSS Preset Selection"
    Expected Result: All flags documented in advanced doc, preset section present in usage guide
    Failure Indicators: Any flag missing from advanced doc
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-5-doc-coverage.txt

  Scenario: No regression on existing tests
    Tool: Bash (PowerShell)
    Preconditions: Full plan merged
    Steps:
      1. Run: ctest -C Release -LE gpu_required --output-on-failure
    Expected Result: 100% pass on non-GPU tests
    Failure Indicators: Any pre-existing test now fails
    Evidence: .sisyphus/evidence/dlss-pipeline-fix/task-5-no-regression.txt
  ```

  **Evidence to Capture**:
  - [ ] task-5-integration-trace.txt (full ctest output for IntegrationPresetTrace)
  - [ ] task-5-doc-coverage.txt
  - [ ] task-5-no-regression.txt
  - [ ] task-5-final-help.txt (final `--help` output for full record)

  **Commit**: YES (5/5)
  - Message: `test(integration): per-quality preset trace verification + docs`
  - Files: `tests/test_integration_preset_trace.cpp`, `tests/CMakeLists.txt`, `docs/usage_guide.md`, `docs/dlss_advanced.md` (no new fixture — reuses `tests/fixtures/sequence/`)
  - Pre-commit: `ctest -C Release -LE gpu_required`

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
>
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback → fix → re-run → present again → wait for okay.

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read this plan end-to-end. For each "Must Have": verify implementation exists (read file, run binary, parse log). For each "Must NOT Have": grep codebase for forbidden patterns (e.g., touched FG files? new third-party deps? changed `IsHDR` default?) — reject with file:line if found. Check evidence files exist in `.sisyphus/evidence/dlss-pipeline-fix/`. Compare deliverables against plan section 4.1 of handoff.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [5/5] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build build --config Release 2>&1 | tee build.log`, assert zero warnings introduced by this plan's files. Run `ctest -C Release --output-on-failure`. Review all changed files (`git diff master --name-only`) for: `// TODO`/`// FIXME` left in, `printf`/`std::cout` debug spam in production paths, hardcoded magic numbers without comments, copy-paste duplication of preset table. Check AI slop: wrapper classes added for "future flexibility", over-commented obvious code, generic names (`data`/`temp`/`val`).
  Output: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high`
  Run on canonical fixture (`tests/fixtures/sequence/` or equivalent). Execute every QA scenario from every task — exact commands, capture stdout/stderr/`nvngx_dlss.log`. Cross-task tests: run with `--quality Performance --preset auto` AND `--quality Quality --preset auto` AND verify NGX log shows different presets per the table. Edge cases: missing `nvngx_dlss.dll`, corrupted camera.json, single-frame "sequence", camera-cut on every frame. Save to `.sisyphus/evidence/dlss-pipeline-fix/final-qa/`.
  Output: `Scenarios [N/N pass] | Cross-Task [N/N] | Edge Cases [N tested] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (`git log --oneline master..HEAD`, `git diff master --stat`). Verify 1:1 — everything in spec was built (no missing), nothing beyond spec was built (no creep). Check "Must NOT do" compliance: no `gui/` files touched? no `blender/` files touched? no `frame_generator.cpp` touched? Detect cross-task contamination: Task N touching Task M's files. Flag unaccounted changes (e.g., random whitespace fixups in unrelated files).
  Output: `Tasks [5/5 compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

One commit per task (5 commits). Conventional commit format. Each commit includes `Refs: .sisyphus/plans/dlss-pipeline-fix.md#TaskN` in body.

- **T1**: `fix(dlss): per-quality preset writes (was uniform — Phase 2 Task 3 walkback)` — `src/dlss/ngx_wrapper.cpp`, `src/cli/config.h`, `src/cli/cli_parser.cpp`, `tests/test_preset_mapping.cpp`. Pre-commit: `cmake --build build --config Release && ctest -C Release -R PresetMapping`
- **T2**: `feat(cli): expose all NGX advanced settings as flags` — `src/cli/config.h`, `src/cli/cli_parser.cpp`, `src/dlss/ngx_wrapper.cpp`, `src/dlss/dlss_sr_processor.cpp`, `tests/test_cli_advanced_flags.cpp`, `docs/usage_guide.md`. Pre-commit: `ctest -C Release -R CliAdvancedFlags`
- **T3**: `feat(pipeline): camera cut detection via translation/rotation thresholds` — `src/pipeline/sequence_processor.cpp`, `src/core/camera_data_loader.cpp` (if needed), `src/cli/config.h`, `src/cli/cli_parser.cpp`, `tests/test_camera_cut_detection.cpp`. Pre-commit: `ctest -C Release -R CameraCut`
- **T4**: `feat(dlss): banner emits DLL version + API version fallback` — `src/dlss/ngx_wrapper.cpp`, `src/cli/cli_parser.cpp` (banner output point), `tests/test_ngx_banner.cpp`. Pre-commit: `dlss-compositor.exe --test-ngx --progress-format json | findstr dlss_dll_version`
- **T5**: `test(integration): per-quality preset trace verification + docs` — `tests/test_integration_preset_trace.cpp`, `docs/usage_guide.md` final pass, `docs/dlss_advanced.md` (new). Pre-commit: full `ctest -C Release`

---

## Success Criteria

### Verification Commands
```powershell
# Build clean
cmake --build build --config Release  # Expected: 0 warnings, 0 errors

# All new tests pass
ctest -C Release --output-on-failure  # Expected: 100% pass

# New flags discoverable
dlss-compositor.exe --help | Select-String -Pattern 'preset-dlaa|preset-quality|preset-balanced|preset-performance|preset-ultra-performance|preset-ultra-quality|feature-flag-hdr|feature-flag-mv-low-res|feature-flag-auto-exposure|enable-output-subrects|exposure-scale|frame-time-delta-ms|dlss-sharpness|enable-dynamic-resolution|camera-cut-translation-threshold|camera-cut-rotation-threshold|no-camera-cut-detection'
# Expected: 17 matches (one per new flag)

# Banner has new fields
dlss-compositor.exe --test-ngx --progress-format json 2>&1 | Select-String -Pattern '"dlss_dll_version"'  # Expected: 1+ match
dlss-compositor.exe --test-ngx --progress-format json 2>&1 | Select-String -Pattern '"ngx_api_version_used"'  # Expected: 1+ match

# Per-quality preset actually differs (integration)
dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/q --scale 2 --quality Quality --preset auto --ngx-log-level verbose
dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/p --scale 2 --quality Performance --preset auto --ngx-log-level verbose
# Expected: nvngx_dlss.log entries differ in preset hint between the two runs

# Backward compatibility — old --preset still works
dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir tmp/legacy --scale 2 --preset K
# Expected: identical output bytes to pre-plan baseline (no behavior change)
```

### Final Checklist
- [x] All "Must Have" items verified present
- [x] All "Must NOT Have" items verified absent (grep check)
- [x] All 5 task QA scenarios produced evidence files
- [x] F1–F4 reviews all APPROVE
- [x] User explicitly okayed final state
