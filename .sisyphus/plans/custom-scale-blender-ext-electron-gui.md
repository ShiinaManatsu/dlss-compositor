# Custom Float Scale + Blender 5.x Extension + Electron GUI

## TL;DR

> **Quick Summary**: Add custom float scale factor support (≥1.0) with DLAA quality mode to DLSS-RR, convert the Blender AOV export script to a proper Blender 5.x extension, and build a full-featured Electron GUI (React + Vite + Tailwind) that wraps the CLI for artist-friendly usage.
> 
> **Deliverables**:
> - Float scale factor (1.0-8.0) with DLAA denoise-only mode in C++ CLI
> - Blender 5.x extension package (`dlss_compositor_aov/`) with manifest
> - Electron desktop app in `gui/` with complete CLI coverage, progress tracking, and professional dark UI
> 
> **Estimated Effort**: Large (3 features, ~18 tasks)
> **Parallel Execution**: YES — 4 waves
> **Critical Path**: Task 1→2→3→4 (Feature 1) → Task 9→10→11→12→13→14 (Feature 3 depends on final CLI contract)

---

## Context

### Original Request
User wants three new features for dlss-compositor:
1. Custom float scale factor (e.g. 1.5x for 720P→1080P) instead of only integer 2/3/4
2. Convert `blender/aov_export_preset.py` to Blender 5.x extension format
3. Build Electron GUI for artist-friendly operation

### Interview Summary
**Key Discussions**:
- Scale ≥1.0 allowed (1.0 = DLAA pure denoise). Quality default: DLAA at 1.0, MaxQuality at >1.0
- Quality mode remains independently configurable via `--quality`
- Offline tool philosophy: quality first, speed second
- Blender 5.0+ only, new extension manifest format
- Electron: React + Tailwind + Vite, complete CLI coverage, target audience is artists
- CLI exe preserved alongside GUI
- Test strategy: Catch2 for C++ changes, Agent QA for Blender + Electron

**Research Findings**:
- DLSS NGX API takes raw pixel dimensions (uint), not scale factors — float works via rounding
- `parseFloat()` helper already exists in cli_parser.cpp:29-41
- `NVSDK_NGX_PerfQuality_Value_DLAA` exists in SDK for denoise-only
- Blender extension schema 1.0.0 stable across 5.0-5.1, bl_info deleted after load
- Existing script already has register()/unregister() and 5.x API compatibility
- No existing Electron setup — complete greenfield
- Progress output is human-readable `"Processing frame %d/%d"` on stdout

### Metis Review
**Identified Gaps** (addressed):
- **ImGui settings panel**: Hardcoded 2x/3x/4x dropdown at `settings_panel.cpp:34-40` needs float input — included in Task 3
- **stdout buffering**: C++ fprintf may buffer, breaking Electron progress parsing — added stdout flush requirement in Task 4 and Task 11
- **Scale=1.0 + interpolation routing**: `scaleFactor > 1` condition at line 587 routes wrong for 1.0 — addressed in Task 3 with explicit float comparison and RRFG denoise-then-interpolate support
- **Electron exe path persistence**: Need `electron-store` for settings — included in Task 9
- **Vulkan alignment**: Output dimensions should be rounded to even — included in resolution calc
- **Scale upper bound**: Capped at 8.0 to prevent VRAM exhaustion

---

## Work Objectives

### Core Objective
Enable flexible DLSS-RR scaling with float factors, modernize the Blender workflow with a proper extension, and provide artists with a professional desktop GUI.

### Concrete Deliverables
- Modified C++ source: `config.h`, `cli_parser.cpp`, `ngx_wrapper.cpp`, `sequence_processor.cpp`, `settings_panel.cpp`
- New Catch2 tests in `tests/test_cli.cpp` for float scale and DLAA
- Blender extension directory: `blender/dlss_compositor_aov/` with `blender_manifest.toml` + `__init__.py`
- Electron app: `gui/` with React+TypeScript+Tailwind, full CLI wrapper
- Updated docs: `README.md`, `docs/usage_guide.md`

### Definition of Done
- [ ] `cmake --build build --config Release` succeeds
- [ ] `build\tests\Release\dlss-compositor-tests.exe` — all tests pass including new float scale tests
- [ ] `dlss-compositor.exe --scale 1.5 --input-dir X --output-dir Y` works correctly
- [ ] `dlss-compositor.exe --scale 1.0 --quality DLAA` works (pure denoise)
- [ ] Blender extension loads in Blender 5.x, panel appears, operators work
- [ ] `npm run dev` in `gui/` launches Electron app
- [ ] GUI can configure all CLI flags and start/stop processing with progress

### Must Have
- Float scale ≥1.0 with backward compatibility for `--scale 2`
- DLAA quality mode support
- Blender 5.x extension format with blender_manifest.toml
- Electron GUI with input/output dir selectors, all CLI options, progress tracking
- Dark professional theme suitable for artists

### Must NOT Have (Guardrails)
- No changes to DLSS FG interpolation factor logic (stays integer 2/4)
- No changes to `createDlssRR()` function signature — it already takes int widths/heights
- No C++20 features — stay on current standard
- No Electron packaging/installer (deferred)
- No auto-update functionality
- No dark/light theme toggle — single dark theme
- No custom UI component library — Tailwind utility classes only
- No second settings page — single-screen layout
- No animation/transition effects beyond Tailwind defaults
- Developer-only CLI flags (`--test-ngx`, `--test-vulkan`, `--test-gui`, `--test-load`, `--test-process`, `--gui`) hidden from Electron GUI
- No Blender marketplace publishing
- No changes to existing operator logic or panel functionality in Blender extension

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.

### Test Decision
- **Infrastructure exists**: YES (Catch2 for C++, pytest for Python)
- **Automated tests**: Tests-after for Feature 1 (Catch2), Agent QA for Features 2 & 3
- **Framework**: Catch2 (C++), Playwright (Electron GUI), BlenderMCP (Blender extension)

### QA Policy
Every task MUST include agent-executed QA scenarios.
Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **C++ CLI**: Use Bash — run exe with flags, assert output/exit code
- **Blender Extension**: Use BlenderMCP — execute operators, verify scene state
- **Electron GUI**: Use Playwright — navigate, interact, assert DOM, screenshot
- **Build verification**: Use Bash — cmake build, check exit codes

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately — Feature 1 foundation + Feature 2 parallel):
├── Task 1: [F1] Add DLAA to quality enum + CLI parsing           [quick]
├── Task 2: [F1] Float scale CLI parsing + validation              [quick]
├── Task 3: [F1] Float scale resolution calc + pipeline routing    [deep]
├── Task 4: [F1] Stdout flush + ImGui settings panel update        [quick]
├── Task 5: [F1] Catch2 tests for float scale + DLAA              [quick]
├── Task 6: [F2] Blender extension manifest + restructure          [quick]
├── Task 7: [F2] BlenderMCP verification + mklink setup            [unspecified-high]
└── Task 8: [F1+F2] Update docs (README, usage_guide)             [writing]

Wave 2 (After Wave 1 — Feature 3 scaffold + IPC):
├── Task 9:  [F3] Electron + React + Vite project scaffold        [quick]
├── Task 10: [F3] IPC layer + child process management            [deep]
└── Task 11: [F3] Progress parsing + stdout stream handling        [unspecified-high]

Wave 3 (After Wave 2 — Feature 3 UI + persistence):
├── Task 12: [F3] Main layout + input/output controls             [visual-engineering]
├── Task 13: [F3] Advanced settings panel                         [visual-engineering]
├── Task 15: [F3] Exe path config + settings persistence          [quick]  (moved earlier — blocks Task 14)
└── Task 14: [F3] Processing view + log + start/stop              [visual-engineering]  (depends on 15 for exe path)

Wave 4 (After Wave 3 — Feature 3 QA + polish):
├── Task 16: [F3] Playwright smoke tests                          [unspecified-high]
└── Task 17: [F3] UI polish + error states + edge cases           [visual-engineering]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real manual QA (unspecified-high)
└── Task F4: Scope fidelity check (deep)
-> Present results -> Get explicit user okay
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 2, 3, 5 | 1 |
| 2 | 1 | 3, 5 | 1 |
| 3 | 1, 2 | 4, 5, 9 | 1 |
| 4 | 3 | 8 | 1 |
| 5 | 1, 2, 3 | 8 | 1 |
| 6 | — | 7 | 1 |
| 7 | 6 | 8 | 1 |
| 8 | 4, 5, 7 | — | 1 |
| 9 | 3 | 10, 12 | 2 |
| 10 | 9 | 11, 15 | 2 |
| 11 | 10 | 14 | 2 |
| 12 | 9 | 13, 14, 15 | 3 |
| 13 | 12 | 16 | 3 |
| 14 | 11, 12, 15 | 16 | 3 |
| 15 | 10, 12 | 14 | 3 |
| 16 | 13, 14 | 17 | 4 |
| 17 | 16 | F1-F4 | 4 |

### Agent Dispatch Summary

- **Wave 1**: **8 tasks** — T1→`quick`, T2→`quick`, T3→`deep`, T4→`quick`, T5→`quick`, T6→`quick`, T7→`unspecified-high`, T8→`writing`
- **Wave 2**: **3 tasks** — T9→`quick`, T10→`deep`, T11→`unspecified-high`
- **Wave 3**: **4 tasks** — T12→`visual-engineering`, T13→`visual-engineering`, T15→`quick`, T14→`visual-engineering` (T14 after T15)
- **Wave 4**: **2 tasks** — T16→`unspecified-high`, T17→`visual-engineering`
- **FINAL**: **4 tasks** — F1→`oracle`, F2→`unspecified-high`, F3→`unspecified-high`, F4→`deep`

---

## TODOs

- [x] 1. [F1] Add DLAA Quality Mode to Enum + CLI Parsing

  **What to do**:
  - Add `DLAA` to `DlssQualityMode` enum in `config.h:10-15` (before MaxQuality)
  - Add `"DLAA"` case to `parseQuality()` in `cli_parser.cpp:43-61`
  - Add `DLAA` case to `mapQuality()` in `ngx_wrapper.cpp:319-331`, mapping to `NVSDK_NGX_PerfQuality_Value_DLAA`
  - Update `--quality` help text in `cli_parser.cpp` to include DLAA option
  - Change default quality in `config.h:47` from `Balanced` to `MaxQuality` (quality-first philosophy; DLAA auto-selected at scale=1.0 in Task 3)

  **Must NOT do**:
  - Do NOT change any other quality mode mappings
  - Do NOT change the `createDlssRR()` function signature
  - Do NOT add C++20 features

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 2, 6)
  - **Blocks**: Tasks 2, 3, 5
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/cli/config.h:10-15` — DlssQualityMode enum, add DLAA as first entry
  - `src/cli/cli_parser.cpp:43-61` — parseQuality() function, add "DLAA" string case
  - `src/dlss/ngx_wrapper.cpp:319-331` — mapQuality(), add DLAA→NVSDK_NGX_PerfQuality_Value_DLAA case

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_defs.h` — Search for `NVSDK_NGX_PerfQuality_Value_DLAA` to confirm SDK enum value exists
  - `src/cli/config.h:47` — Default quality mode to change

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: DLAA quality mode parses correctly
    Tool: Bash
    Preconditions: Project built with cmake
    Steps:
      1. Build: cmake --build build --config Release
      2. Run: build\Release\dlss-compositor.exe --quality DLAA --help
      3. Verify help text includes "DLAA" in quality options
    Expected Result: Build succeeds, help text lists DLAA
    Evidence: .sisyphus/evidence/task-1-dlaa-parse.txt

  Scenario: Invalid quality mode rejected
    Tool: Bash
    Steps:
      1. Run: build\Release\dlss-compositor.exe --quality FooBar --input-dir . --output-dir .
      2. Check exit code is non-zero
      3. Check stderr contains error about invalid quality
    Expected Result: Exit code 1, error message about invalid quality
    Evidence: .sisyphus/evidence/task-1-quality-error.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add DLAA quality mode to enum and parser`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`, `src/dlss/ngx_wrapper.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 2. [F1] Float Scale Factor CLI Parsing + Validation

  **What to do**:
  - Change `int scaleFactor = 2` to `float scaleFactor = 2.0f` in `config.h:45`
  - In `cli_parser.cpp:217-229`: Replace `parseInt()` with existing `parseFloat()` helper (at line 29-41)
  - Change validation from `scale != 2 && scale != 3 && scale != 4` to `scale < 1.0f || scale > 8.0f`
  - Update error message to `"--scale must be between 1.0 and 8.0"`
  - Update `--scale` help text (around line 409) to show `"(float, 1.0–8.0; default 2.0)"`

  **Must NOT do**:
  - Do NOT change the resolution calculation yet (that's Task 3)
  - Do NOT change `scaleExplicit` behavior — it stays bool

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 6, after Task 1)
  - **Parallel Group**: Wave 1
  - **Blocks**: Tasks 3, 5
  - **Blocked By**: Task 1 (needs DLAA enum first for default quality logic)

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:29-41` — Existing `parseFloat()` helper to reuse
  - `src/cli/cli_parser.cpp:217-229` — Current `--scale` parsing block to modify
  - `src/cli/config.h:45` — `int scaleFactor = 2` to change to float

  **API/Type References**:
  - `src/cli/config.h:41-76` — Full AppConfig struct, scaleFactor is the field to change

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Float scale 1.5 accepted
    Tool: Bash
    Steps:
      1. Build: cmake --build build --config Release
      2. Run: build\Release\dlss-compositor.exe --scale 1.5 --help
      3. Verify no parse error, help prints normally
    Expected Result: Exit 0, help output shown
    Evidence: .sisyphus/evidence/task-2-float-scale.txt

  Scenario: Scale below minimum rejected
    Tool: Bash
    Steps:
      1. Run: build\Release\dlss-compositor.exe --scale 0.5 --input-dir . --output-dir .
      2. Check exit code non-zero
      3. Check stderr contains error about scale range
    Expected Result: Exit 1, error about scale range
    Evidence: .sisyphus/evidence/task-2-scale-error.txt

  Scenario: Integer scale 2 still works (backward compat)
    Tool: Bash
    Steps:
      1. Run: build\Release\dlss-compositor.exe --scale 2 --help
      2. Verify no error
    Expected Result: Exit 0, parsed as 2.0f
    Evidence: .sisyphus/evidence/task-2-backward-compat.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): support float scale factor 1.0-8.0`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 3. [F1] Float Scale Resolution Calculation + Pipeline Routing

  **What to do**:
  - In `sequence_processor.cpp:633-634` (RR-only path): Change `expectedInputWidth * config.scaleFactor` to `static_cast<int>(std::round(static_cast<double>(expectedInputWidth) * config.scaleFactor))` — same for height. Ensure result is even: `(result + 1) & ~1` if odd (Vulkan alignment safety)
  - Same change at `sequence_processor.cpp:954-955` (RRFG combined path)
  - Add `#include <cmath>` at top of sequence_processor.cpp if not present
  - **Routing rule (single source of truth)**: At `sequence_processor.cpp:587`, replace `config.scaleFactor > 1` with `config.scaleFactor >= 1.0f`. This means:
    - `scaleFactor >= 1.0` with `interpolateFactor == 0` → RR-only (denoise/upscale)
    - `scaleFactor >= 1.0` with `interpolateFactor > 0` → RRFG (denoise/upscale then interpolate)
    - `scaleFactor == 0` (or unset) with `interpolateFactor > 0` → FG-only (existing path, NO changes needed — the existing `else` branch handles this)
    - The default `scaleFactor = 2.0f` means users who only pass `--interpolate` without `--scale` will get RRFG. To get FG-only, user must explicitly pass `--scale 0` or we keep the existing behavior where `scaleFactor` defaults to 0 when not provided. **Decision**: Change default from `2.0f` to `0.0f`, so `--scale` is opt-in. When `--scale` is provided (any value ≥ 1.0), RR is activated. When `--scale` is not provided, `scaleFactor` stays 0 and the existing FG-only path works unchanged.
  - At `config.h:45`: Change default from `float scaleFactor = 2.0f;` to `float scaleFactor = 0.0f;` and add `bool scaleExplicit = false;`
  - At `cli_parser.cpp` `--scale` parsing: set `config.scaleExplicit = true;` and validate value ≥ 1.0 only when explicitly provided
  - At `sequence_processor.cpp:601`: Change `config.scaleFactor <= 0` guard to `config.scaleExplicit && config.scaleFactor < 1.0f` (error only if user explicitly passed an invalid value)
  - At `sequence_processor.cpp:852`: Change printf format from `scale=%d` to `scale=%.2f` for scaleFactor
  - Update ImGui `settings_panel.cpp:34-40`: Replace hardcoded `{"2x","3x","4x"}` Combo with `ImGui::InputFloat("Scale", &scaleFactor, 0.1f, 0.5f, "%.1f")` with `ImGui::ClampFloat` min=1.0 max=8.0

  **Must NOT do**:
  - Do NOT change `createDlssRR()` signature — it takes int widths/heights already
  - Do NOT change motion vector conversion logic
  - Do NOT change FG interpolation factor logic

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (sequential after Tasks 1+2)
  - **Parallel Group**: Wave 1 (sequential within wave)
  - **Blocks**: Tasks 4, 5, 9
  - **Blocked By**: Tasks 1, 2

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:587` — RRFG routing condition `config.scaleFactor > 1`
  - `src/pipeline/sequence_processor.cpp:601` — Scale validation guard
  - `src/pipeline/sequence_processor.cpp:633-634` — RR-only resolution calc
  - `src/pipeline/sequence_processor.cpp:852` — Printf format string
  - `src/pipeline/sequence_processor.cpp:954-955` — RRFG combined resolution calc
  - `src/ui/settings_panel.cpp:34-40` — ImGui scale dropdown to replace

  **API/Type References**:
  - `src/dlss/ngx_wrapper.h:33-39` — `createDlssRR()` takes int widths — round float result before passing

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Resolution calc at 1.5x from 64x64 fixture
    Tool: Bash
    Steps:
      1. Build project: cmake --build build --config Release
      2. Run: build\Release\dlss-compositor.exe --scale 1.5 --input-dir tests\fixtures\sequence --output-dir test_out --quality MaxQuality
      3. Check stdout for output resolution line (should show 96x96 — round(64*1.5)=96, already even)
    Expected Result: Output dimensions = 96x96 (1.5x of 64x64 fixture)
    Evidence: .sisyphus/evidence/task-3-resolution-calc.txt

  Scenario: Scale 1.0 pure denoise
    Tool: Bash
    Steps:
      1. Run: build\Release\dlss-compositor.exe --scale 1.0 --input-dir tests\fixtures\sequence --output-dir test_out
      2. Check output resolution matches input resolution (64x64)
    Expected Result: Output dimensions = 64x64 (no upscale, denoise only)
    Evidence: .sisyphus/evidence/task-3-scale-1-denoise.txt

  Scenario: Build succeeds with float scale changes
    Tool: Bash
    Steps:
      1. cmake --build build --config Release
      2. Verify exit code 0
    Expected Result: Clean build
    Evidence: .sisyphus/evidence/task-3-build.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): float scale resolution calc + RRFG routing for scale ≥1.0`
  - Files: `src/pipeline/sequence_processor.cpp`, `src/ui/settings_panel.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 4. [F1] Stdout Flush for Progress + Float Printf Format

  **What to do**:
  - In `sequence_processor.cpp`, after every `std::fprintf(stdout, ...)` progress line (search for `"Processing frame"` pattern), add `std::fflush(stdout);`
  - Also add flush after the scale/config summary printf (around line 852)
  - This is critical for Electron GUI (Task 11) to receive real-time progress updates
  - Verify all scaleFactor printf formats use `%.2f` not `%d`

  **Must NOT do**:
  - Do NOT set global stdout unbuffered (affects performance)
  - Do NOT change the format of progress messages (Electron will parse them)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Task 3)
  - **Parallel Group**: Wave 1 (sequential)
  - **Blocks**: Task 8
  - **Blocked By**: Task 3

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp` — Search for all `fprintf(stdout` calls, add `fflush(stdout)` after each progress line

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Build succeeds with flush additions
    Tool: Bash
    Steps:
      1. cmake --build build --config Release
      2. Verify exit 0
    Expected Result: Clean build
    Evidence: .sisyphus/evidence/task-4-build.txt

  Scenario: Verify fflush calls present in source
    Tool: Grep
    Steps:
      1. grep for "fflush(stdout)" in sequence_processor.cpp
      2. Verify count ≥ number of progress fprintf calls
    Expected Result: fflush present after each progress line
    Evidence: .sisyphus/evidence/task-4-flush-verify.txt
  ```

  **Commit**: YES
  - Message: `fix(pipeline): flush stdout after progress lines for GUI consumption`
  - Files: `src/pipeline/sequence_processor.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 5. [F1] Catch2 Tests for Float Scale + DLAA

  **What to do**:
  - Add new test cases to `tests/test_cli.cpp` using existing `FakeArgs` pattern:
    - `--scale 1.5` → `cfg.scaleFactor == Catch::Approx(1.5f)`, `cfg.scaleExplicit == true`
    - `--scale 1.0` → `cfg.scaleFactor == Catch::Approx(1.0f)`
    - `--scale 0.5` → parse failure (below minimum)
    - `--scale 9.0` → parse failure (above maximum)
    - `--scale 2.0` → `cfg.scaleFactor == Catch::Approx(2.0f)` (backward compat)
    - `--scale 2` → `cfg.scaleFactor == Catch::Approx(2.0f)` (integer input still works)
    - `--scale abc` → parse failure
    - `--quality DLAA` → `cfg.quality == DlssQualityMode::DLAA`
    - `--quality dlaa` → parse failure (case-sensitive, consistent with existing behavior)
  - Use `Catch::Approx` for float comparisons
  - Follow existing test patterns in test_cli.cpp

  **Must NOT do**:
  - Do NOT modify existing passing tests
  - Do NOT add tests for resolution calculation here (that's integration-level)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Tasks 1, 2, 3)
  - **Parallel Group**: Wave 1 (sequential)
  - **Blocks**: Task 8
  - **Blocked By**: Tasks 1, 2, 3

  **References**:

  **Pattern References**:
  - `tests/test_cli.cpp` — Full test file, study `FakeArgs` helper class and existing test structure
  - `tests/CMakeLists.txt` — Test build config, no changes needed (test_cli.cpp already included)

  **Test References**:
  - `tests/test_cli.cpp` — Search for existing `--scale` tests (currently test integer 2/3/4) and `--quality` tests

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: All tests pass including new float scale tests
    Tool: Bash
    Steps:
      1. cmake --build build --config Release
      2. Run: build\tests\Release\dlss-compositor-tests.exe --reporter compact
      3. Verify all tests pass, new test count > old count
    Expected Result: All assertions passed, 0 failures
    Evidence: .sisyphus/evidence/task-5-tests.txt

  Scenario: Float scale boundary tests
    Tool: Bash
    Steps:
      1. Run: build\tests\Release\dlss-compositor-tests.exe "[cli]" --reporter compact
      2. Verify float scale tests specifically pass
    Expected Result: CLI test section all green
    Evidence: .sisyphus/evidence/task-5-cli-tests.txt
  ```

  **Commit**: YES
  - Message: `test(cli): add Catch2 tests for float scale factor and DLAA quality`
  - Files: `tests/test_cli.cpp`
  - Pre-commit: `build\tests\Release\dlss-compositor-tests.exe`

- [x] 6. [F2] Blender Extension Manifest + Code Restructure

  **What to do**:
  - Create directory `blender/dlss_compositor_aov/`
  - Create `blender/dlss_compositor_aov/blender_manifest.toml`:
    ```toml
    schema_version = "1.0.0"
    id = "dlss_compositor_aov"
    version = "1.0.0"
    name = "DLSS Compositor AOV Export"
    tagline = "Configure render passes and AOVs for DLSS Compositor"
    type = "add-on"
    blender_version_min = "5.0.0"
    license = ["SPDX:MIT"]
    maintainer = "DLSS Compositor"
    tags = ["Render"]
    permissions = ["files"]

    [build]
    paths_exclude_pattern = ["__pycache__/", "*.pyc"]
    ```
  - Copy `blender/aov_export_preset.py` to `blender/dlss_compositor_aov/__init__.py`
  - In the new `__init__.py`: Remove the `bl_info` dict (lines 9-17)
  - Keep the `register()`, `unregister()`, and `__main__` blocks exactly as-is
  - Remove Blender 4.x compatibility branches since min version is 5.0 (the `bpy.app.version >= (5, 0, 0)` checks — keep only the 5.x code path for each branch)
  - Keep original `blender/aov_export_preset.py` as-is for reference (don't delete)
  - Create symlink: `mklink /D "F:\Projects\Blender\Extensions\dlss_compositor_aov" "F:\Projects\GitRepos\dlss-compositor\blender\dlss_compositor_aov"`

  **Must NOT do**:
  - Do NOT change any operator logic, panel layout, or button functionality
  - Do NOT add new features to the extension
  - Do NOT delete the original aov_export_preset.py

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (independent of Feature 1)
  - **Parallel Group**: Wave 1 (parallel with Tasks 1, 2)
  - **Blocks**: Task 7
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `blender/aov_export_preset.py` — Full source to migrate (426 lines). Lines 9-17 = bl_info to remove. Lines 385-398 = register/unregister to keep. Lines 405-426 = __main__ to keep.

  **External References**:
  - Blender 5.x extension schema: https://developer.blender.org/docs/features/extensions/schema/1.0.0/
  - Extension getting started: https://docs.blender.org/manual/en/latest/advanced/extensions/getting_started.html

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Extension directory structure correct
    Tool: Bash
    Steps:
      1. Verify blender/dlss_compositor_aov/blender_manifest.toml exists
      2. Verify blender/dlss_compositor_aov/__init__.py exists
      3. Verify __init__.py does NOT contain "bl_info"
      4. Verify blender_manifest.toml contains schema_version = "1.0.0"
    Expected Result: All files present, bl_info removed, manifest valid
    Evidence: .sisyphus/evidence/task-6-structure.txt

  Scenario: Headless test still works
    Tool: Bash
    Steps:
      1. Run: "D:\Games\SteamLibrary\steamapps\common\Blender\blender.exe" --background --factory-startup --python blender\dlss_compositor_aov\__init__.py -- --test
      2. Check exit code 0
    Expected Result: Exit 0, passes configured message printed
    Evidence: .sisyphus/evidence/task-6-headless.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): convert AOV export to Blender 5.x extension format`
  - Files: `blender/dlss_compositor_aov/blender_manifest.toml`, `blender/dlss_compositor_aov/__init__.py`
  - Pre-commit: Blender headless test

- [x] 7. [F2] BlenderMCP Verification + Functional Test

  **What to do**:
  - Using BlenderMCP, verify the extension loads correctly in Blender 5.x:
    1. Verify extension appears in Blender Preferences → Add-ons
    2. Verify "DLSS Compositor" panel appears in Render Properties
    3. Run `bpy.ops.dlsscomp.configure_passes()` — verify it enables passes
    4. Set output directory, run `bpy.ops.dlsscomp.export_camera()` — verify camera.json created
    5. Check pass status shows all passes ON
  - Verify symlink works: `F:\Projects\Blender\Extensions\dlss_compositor_aov` → `blender\dlss_compositor_aov`
  - Fix any issues found during verification

  **Must NOT do**:
  - Do NOT add new features during verification
  - Do NOT change panel layout unless something is broken

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Task 6)
  - **Parallel Group**: Wave 1 (sequential after Task 6)
  - **Blocks**: Task 8
  - **Blocked By**: Task 6

  **References**:

  **Pattern References**:
  - `blender/dlss_compositor_aov/__init__.py` — The extension code to verify
  - `blender/aov_export_preset.py` — Original reference to compare behavior against

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Extension loads and panel visible
    Tool: BlenderMCP
    Steps:
      1. Open Blender with the extension enabled
      2. Navigate to Render Properties panel
      3. Verify "DLSS Compositor" section exists
      4. Verify "Configure All Passes" button visible
      5. Verify "Export Camera Data" button visible
    Expected Result: All UI elements present
    Evidence: .sisyphus/evidence/task-7-panel-visible.txt

  Scenario: Configure passes operator works
    Tool: BlenderMCP
    Steps:
      1. Execute: bpy.ops.dlsscomp.configure_passes()
      2. Check return value is {'FINISHED'}
      3. Verify view_layer.use_pass_combined == True
      4. Verify view_layer.use_pass_z == True
      5. Verify view_layer.use_pass_vector == True
      6. Verify view_layer.use_pass_normal == True
      7. Verify "Roughness" AOV exists in view_layer.aovs
    Expected Result: All passes enabled, Roughness AOV created
    Evidence: .sisyphus/evidence/task-7-passes-configured.txt

  Scenario: Camera export operator works
    Tool: BlenderMCP
    Steps:
      1. Set scene.dlsscomp_output_dir to a temp directory
      2. Add a camera to scene
      3. Execute: bpy.ops.dlsscomp.export_camera()
      4. Verify camera.json file exists at output path
      5. Verify JSON contains "version", "render_width", "frames" keys
    Expected Result: camera.json created with valid structure
    Evidence: .sisyphus/evidence/task-7-camera-export.txt
  ```

  **Commit**: YES (if fixes needed)
  - Message: `test(blender): verify extension via BlenderMCP`
  - Files: `.sisyphus/evidence/task-7-*`

- [x] 8. [F1+F2] Update Documentation

  **What to do**:
  - Update `README.md`:
    - CLI Usage section: Update `--scale` examples to show float (e.g., `--scale 1.5`)
    - Add DLAA quality mode example: `--quality DLAA`
    - Add denoise-only example: `--scale 1.0 --quality DLAA`
    - Update Quick Start step 2: Replace "Run Script" instruction with extension installation
    - Mention extension symlink approach for development
  - Update `docs/usage_guide.md`:
    - `--scale` description: "Float value ≥1.0 (default: 2.0). Use 1.0 for denoise-only (DLAA)"
    - `--quality` description: Add DLAA to the list
    - Add section on custom scale factors with common examples (1.5x for 720→1080, etc.)

  **Must NOT do**:
  - Do NOT update docs/build_guide.md for Electron (no packaging yet)
  - Do NOT add Electron GUI documentation yet (Task 17)

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Tasks 4, 5, 7)
  - **Parallel Group**: Wave 1 (final task of wave)
  - **Blocks**: None
  - **Blocked By**: Tasks 4, 5, 7

  **References**:

  **Pattern References**:
  - `README.md` — Current CLI usage section and Quick Start guide
  - `docs/usage_guide.md` — Current flag descriptions

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: README mentions float scale
    Tool: Grep
    Steps:
      1. Search README.md for "--scale 1.5" or "float"
      2. Search README.md for "DLAA"
      3. Search README.md for "extension" in Quick Start section
    Expected Result: All three terms found
    Evidence: .sisyphus/evidence/task-8-readme.txt

  Scenario: Usage guide updated
    Tool: Grep
    Steps:
      1. Search docs/usage_guide.md for "1.0" in scale description
      2. Search for "DLAA" in quality section
    Expected Result: Both found
    Evidence: .sisyphus/evidence/task-8-usage-guide.txt
  ```

  **Commit**: YES
  - Message: `docs: update README and usage guide for float scale, DLAA, and extension install`
  - Files: `README.md`, `docs/usage_guide.md`

- [x] 9. [F3] Electron + React + Vite Project Scaffold

  **What to do**:
  - Create `gui/` directory at project root
  - Initialize with `npm create electron-vite` or manual setup:
    - `package.json` with electron, vite, react, react-dom, @types/react, typescript, tailwindcss, postcss, autoprefixer, electron-store
    - `vite.config.ts` — React plugin + electron plugin config
    - `tailwind.config.js` — content paths, dark mode class
    - `postcss.config.js` — tailwindcss + autoprefixer
    - `tsconfig.json` — strict mode, React JSX
  - Directory structure:
    ```
    gui/
    ├── electron/
    │   ├── main.ts          # Electron main process
    │   └── preload.ts       # Context bridge
    ├── src/
    │   ├── App.tsx           # Root component
    │   ├── main.tsx          # React entry
    │   ├── index.css         # Tailwind imports
    │   ├── components/       # UI components (empty for now)
    │   ├── hooks/            # Custom hooks (empty for now)
    │   ├── ipc/              # IPC type definitions
    │   └── types/            # Shared types
    ├── index.html
    ├── package.json
    ├── vite.config.ts
    ├── tailwind.config.js
    ├── tsconfig.json
    └── tsconfig.node.json
    ```
  - Basic App.tsx with dark background, app title "DLSS Compositor", placeholder text
  - Verify `npm install` and `npm run dev` work

  **Must NOT do**:
  - Do NOT add any UI components yet (Tasks 12-14)
  - Do NOT add IPC implementation (Task 10)
  - Do NOT add animation or theme toggle
  - Do NOT create custom component library

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Feature 1 complete = Task 3, to know final CLI contract)
  - **Parallel Group**: Wave 2 start
  - **Blocks**: Tasks 10, 12
  - **Blocked By**: Task 3

  **References**:

  **External References**:
  - electron-vite: https://electron-vite.org/ — Official Vite for Electron setup guide
  - electron-store: https://github.com/sindresorhus/electron-store — Persistent settings storage
  - Tailwind CSS: https://tailwindcss.com/docs/installation — PostCSS setup

  **Pattern References**:
  - `src/cli/config.h:41-76` — AppConfig struct defines ALL CLI flags that the GUI must expose. This is the source of truth for the GUI's settings model.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: npm install succeeds
    Tool: Bash (workdir=gui/)
    Steps:
      1. cd gui && npm install
      2. Verify exit code 0
      3. Verify node_modules/ exists
    Expected Result: All deps installed
    Evidence: .sisyphus/evidence/task-9-npm-install.txt

  Scenario: Dev server launches
    Tool: Bash (workdir=gui/)
    Steps:
      1. npm run dev (run for 10 seconds then kill)
      2. Check stdout for "Electron" launch message
    Expected Result: Electron window attempted to open
    Evidence: .sisyphus/evidence/task-9-dev-launch.txt

  Scenario: Build produces output
    Tool: Bash (workdir=gui/)
    Steps:
      1. npm run build
      2. Verify gui/dist/ exists
      3. Verify gui/dist-electron/ exists
    Expected Result: Build artifacts created
    Evidence: .sisyphus/evidence/task-9-build.txt
  ```

  **Commit**: YES
  - Message: `chore(gui): scaffold Electron + React + Vite + Tailwind project`
  - Files: `gui/*`
  - Pre-commit: `cd gui && npm run build`

- [x] 10. [F3] IPC Layer + Child Process Management

  **What to do**:
  - Define TypeScript types for CLI config in `gui/src/types/dlss-config.ts`:
    ```typescript
    interface DlssConfig {
      inputDir: string;
      outputDir: string;
      scaleEnabled: boolean;     // true = include --scale in CLI args; false = omit (FG-only mode)
      scaleFactor: number;       // 1.0-8.0, only used when scaleEnabled=true
      quality: 'DLAA' | 'MaxQuality' | 'Balanced' | 'Performance' | 'UltraPerformance';
      interpolateFactor: 0 | 2 | 4;
      cameraDataFile: string;
      memoryBudgetGB: number;
      encodeVideo: boolean;
      videoOutputFile: string;
      fps: number;
      exrCompression: 'none' | 'zip' | 'zips' | 'piz' | 'dwaa' | 'dwab';
      exrDwaQuality: number;
      outputPasses: string[];    // ['beauty', 'depth', 'normals']
      tonemapMode: 'none' | 'pq';
      inverseTonemapEnabled: boolean;
      forwardLutFile: string;
      inverseLutFile: string;
      channelMapFile: string;
    }
    ```
  - Implement `gui/electron/process-manager.ts`:
    - `buildCliArgs(config: DlssConfig, exePath: string): string[]` — convert config to CLI args array. When `config.scaleEnabled === false`, omit `--scale` and `--quality` from args (FG-only mode). When `config.scaleEnabled === true`, include `--scale {scaleFactor}` and `--quality {quality}`.
    - `startProcess(exePath: string, args: string[]): ChildProcess` — spawn with `stdio: ['ignore', 'pipe', 'pipe']`
    - `killProcess(proc: ChildProcess): void` — send SIGTERM/taskkill
    - Handle `app.on('before-quit')` to kill orphaned child processes
  - Implement `gui/electron/preload.ts`:
    - `contextBridge.exposeInMainWorld('dlssApi', { ... })` with:
      - `selectDirectory(): Promise<string>` — dialog.showOpenDialog
      - `selectFile(filters): Promise<string>` — dialog.showOpenDialog for files
      - `startProcessing(config: DlssConfig): void` — IPC to main
      - `stopProcessing(): void` — IPC to main
      - `onProgress(callback): void` — receive progress updates
      - `onError(callback): void` — receive error messages
      - `onComplete(callback): void` — receive completion
      - `getSettings(): Promise<Settings>` — load persisted settings
      - `saveSettings(settings): Promise<void>` — persist settings
  - Type-safe IPC channel definitions in `gui/src/ipc/channels.ts`

  **Must NOT do**:
  - Do NOT implement UI components (Tasks 12-14)
  - Do NOT add progress parsing yet (Task 11)
  - Do NOT hardcode exe path — it comes from settings

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Task 9)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 11
  - **Blocked By**: Task 9

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:395-436` — Help text shows exact flag names and accepted values. This is the authoritative reference for `buildCliArgs()` mapping.

  **API/Type References**:
  - `src/cli/config.h:41-76` — AppConfig struct. The TypeScript DlssConfig must mirror this 1:1.

  **External References**:
  - Electron contextBridge: https://www.electronjs.org/docs/latest/api/context-bridge
  - Electron IPC: https://www.electronjs.org/docs/latest/tutorial/ipc

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: CLI args builder produces correct output
    Tool: Bash
    Steps:
      1. cd gui && npm run build
      2. Create temporary test script gui/test-args.mjs:
         ```
         import { buildCliArgs } from './dist-electron/process-manager.js';
         const args = buildCliArgs({ scaleFactor: 1.5, quality: 'MaxQuality', inputDir: 'C:\\test', outputDir: 'C:\\out' }, 'C:\\dlss.exe');
         const expected = ['--scale', '1.5', '--quality', 'MaxQuality', '--input-dir', 'C:\\test', '--output-dir', 'C:\\out'];
         const ok = expected.every(v => args.includes(v));
         console.log(ok ? 'PASS' : 'FAIL: ' + JSON.stringify(args));
         process.exit(ok ? 0 : 1);
         ```
      3. Run: node gui/test-args.mjs
      4. Verify exit code 0 and output contains "PASS"
      5. Delete gui/test-args.mjs
    Expected Result: Exit code 0, stdout contains "PASS"
    Failure Indicators: Exit code 1 or "FAIL" in output
    Evidence: .sisyphus/evidence/task-10-args-builder.txt

  Scenario: Preload exposes dlssApi
    Tool: Playwright
    Steps:
      1. Launch Electron app via `npm run dev` in gui/
      2. In Electron renderer, evaluate: `JSON.stringify(Object.keys(window.dlssApi).sort())`
      3. Assert result includes at minimum: "getSettings", "onComplete", "onError", "onProgress", "saveSettings", "selectDirectory", "selectFile", "startProcessing", "stopProcessing"
      4. Evaluate: `typeof window.dlssApi.selectDirectory` — assert equals "function"
    Expected Result: dlssApi object exists with all 9+ methods as functions
    Evidence: .sisyphus/evidence/task-10-preload-api.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): add IPC layer and child process management`
  - Files: `gui/electron/*`, `gui/src/ipc/*`, `gui/src/types/*`
  - Pre-commit: `cd gui && npm run build`

- [x] 11. [F3] Progress Parsing + Stdout Stream Handling

  **What to do**:
  - Implement `gui/electron/progress-parser.ts`:
    - Parse stdout lines matching `"Processing frame (\d+)/(\d+)"` → `{ current: number, total: number }`
    - Parse `"[RRFG]"` / `"[FG]"` prefixes to detect processing mode
    - Parse stderr lines as error messages
    - Handle partial line buffering (stdout may split mid-line)
  - Create React hook `gui/src/hooks/useProcessing.ts`:
    - State: `{ status: 'idle' | 'running' | 'done' | 'error', progress: number, currentFrame: number, totalFrames: number, log: string[], errors: string[] }`
    - `start(config: DlssConfig)` — calls IPC startProcessing
    - `stop()` — calls IPC stopProcessing
    - Listens to `onProgress`, `onError`, `onComplete` callbacks
  - Wire progress events through IPC: main process emits parsed progress → preload relays → renderer hook consumes
  - Ensure stdout `data` events are split by newline and handle partial buffers

  **Must NOT do**:
  - Do NOT change C++ stdout format (already handled in Task 4)
  - Do NOT implement UI progress bar (Task 14)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (after Task 10)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 14
  - **Blocked By**: Task 10

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp` — Search for `fprintf(stdout` to find exact progress format strings:
    - `"Processing frame %d/%d: %s\n"` — main progress line (around line 678)
    - `"[RRFG]"` prefix format (around line 852)

  **External References**:
  - Node.js child_process streams: https://nodejs.org/api/child_process.html#subprocessstdout

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Progress regex matches real output
    Tool: Bash
    Steps:
      1. Create temporary test script gui/test-progress.mjs:
         ```
         import { parseProgressLine } from './dist-electron/progress-parser.js';
         const result = parseProgressLine('Processing frame 5/100: frame_0005.exr');
         const ok = result && result.current === 5 && result.total === 100;
         console.log(ok ? 'PASS' : 'FAIL: ' + JSON.stringify(result));
         process.exit(ok ? 0 : 1);
         ```
      2. Run: cd gui && npm run build
      3. Run: node gui/test-progress.mjs
      4. Verify exit code 0 and stdout contains "PASS"
      5. Delete gui/test-progress.mjs
    Expected Result: Exit code 0, regex parsed current=5 total=100
    Failure Indicators: Exit code 1 or result is null/wrong values
    Evidence: .sisyphus/evidence/task-11-regex.txt

  Scenario: Partial line buffering handled
    Tool: Bash
    Steps:
      1. Create temporary test script gui/test-buffer.mjs:
         ```
         import { ProgressLineBuffer } from './dist-electron/progress-parser.js';
         const buf = new ProgressLineBuffer();
         let lines = [];
         buf.onLine = (line) => lines.push(line);
         buf.feed('Processing fra');
         buf.feed('me 3/10: test.exr\nProcessing frame 4/10: test2.exr\n');
         const ok = lines.length === 2 && lines[0] === 'Processing frame 3/10: test.exr' && lines[1] === 'Processing frame 4/10: test2.exr';
         console.log(ok ? 'PASS' : 'FAIL: ' + JSON.stringify(lines));
         process.exit(ok ? 0 : 1);
         ```
      2. Run: node gui/test-buffer.mjs
      3. Verify exit code 0 and stdout contains "PASS"
      4. Delete gui/test-buffer.mjs
    Expected Result: Exit code 0, partial chunks correctly reassembled into 2 complete lines
    Failure Indicators: Exit code 1 or wrong line count/content
    Evidence: .sisyphus/evidence/task-11-buffer.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): progress parsing and stdout stream handler`
  - Files: `gui/electron/progress-parser.ts`, `gui/src/hooks/useProcessing.ts`
  - Pre-commit: `cd gui && npm run build`

- [x] 12. [F3] Main Layout + Input/Output Controls

  **What to do**:
  - Create `gui/src/App.tsx` as the root layout:
    - Single-screen layout: left panel (settings), right panel (processing view)
    - Dark theme via Tailwind: `bg-gray-900 text-gray-100` base
    - App title bar with version display
  - Create `gui/src/components/InputOutputPanel.tsx`:
    - **Input Directory**: text field + "Browse" button calling `window.dlssApi.selectDirectory()`
    - **Output Directory**: text field + "Browse" button
    - Directory fields show truncated path with tooltip for full path
    - Validate: show red border + error text if directory doesn't exist or is empty string
  - Create `gui/src/components/BasicSettings.tsx`:
    - **Enable Upscaling (DLSS-RR)**: checkbox `[data-testid="scale-enabled"]`, default checked. When unchecked, scale factor and quality mode are hidden/disabled (FG-only mode — only interpolation is used). When checked, scale + quality controls are visible.
    - **Scale Factor**: number input field with 0.1 step, min=1.0, max=8.0, default=2.0. Only visible/enabled when "Enable Upscaling" is checked.
    - **Quality Mode**: dropdown — Ultra Performance, Performance, Balanced, Max Quality, DLAA (auto-selects DLAA when scale=1.0, MaxQuality when scale>1.0 — user can override). Only visible/enabled when "Enable Upscaling" is checked.
    - **Interpolation**: dropdown — None, 2x, 4x (default: None)
    - **Camera Data File**: file picker (enabled only when interpolation != None), filters: `*.json`
    - **Encode Video**: checkbox, when checked shows:
      - FPS: number input (default 24)
      - Output filename: text input (default "output.mp4")
    - **Validation**: At least one of "Enable Upscaling" or "Interpolation != None" must be true. Show inline error if both are disabled/None: "Select at least upscaling or interpolation"
  - Create `gui/src/components/Layout.tsx`:
    - Responsive flexbox: sidebar 380px fixed, main area fills rest
    - Scroll overflow on sidebar if content exceeds viewport height
    - Minimum window size enforced in `main.ts`: 1024x768
  - Create `gui/src/state/config-store.ts`:
    - React Context + useReducer for centralized config state
    - Type: `DlssConfig` from `gui/src/types/dlss-config.ts` (created in Task 10)
    - Actions: `SET_SCALE_ENABLED`, `SET_INPUT_DIR`, `SET_OUTPUT_DIR`, `SET_SCALE`, `SET_QUALITY`, `SET_INTERPOLATION`, etc.
    - Auto-quality logic: dispatch `SET_QUALITY` to DLAA when scale set to 1.0, to MaxQuality when scale set >1.0 (only if quality was not manually overridden)
    - Default state: `scaleEnabled: true, scaleFactor: 2.0, quality: 'MaxQuality'`

  **Must NOT do**:
  - Do NOT implement advanced settings (Task 13)
  - Do NOT implement processing/log view (Task 14)
  - Do NOT add electron-store persistence (Task 15)
  - Do NOT use any component library — Tailwind utility classes only

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 11)
  - **Parallel Group**: Wave 3 (with Tasks 11, 12)
  - **Blocks**: Tasks 13, 14
  - **Blocked By**: Task 9

  **References**:

  **Pattern References**:
  - `gui/src/types/dlss-config.ts` — DlssConfig type (created in Task 10). Use this for config state shape.
  - `gui/electron/preload.ts` — `window.dlssApi.selectDirectory()` and `window.dlssApi.selectFile()` (created in Task 10). These are the dialog APIs.

  **API/Type References**:
  - `src/cli/config.h:10-15` — DlssQualityMode enum: UltraPerformance, Performance, Balanced, MaxQuality, DLAA (DLAA added by Task 1). Dropdown must match these exactly.
  - `src/cli/config.h:41-76` — AppConfig struct. Scale is `float scaleFactor` (≥1.0), interpolation is `InterpolationMode`.

  **External References**:
  - Tailwind CSS dark mode: https://tailwindcss.com/docs/dark-mode
  - React useReducer: https://react.dev/reference/react/useReducer

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Main layout renders with sidebar and main area
    Tool: Playwright
    Steps:
      1. Launch Electron app via `npm run dev` in gui/
      2. Wait for window to load (selector: '[data-testid="app-root"]')
      3. Assert sidebar panel exists: '[data-testid="settings-panel"]'
      4. Assert main area exists: '[data-testid="main-area"]'
      5. Verify sidebar width is approximately 380px (getBoundingClientRect)
      6. Screenshot full window
    Expected Result: Two-panel layout visible, dark theme applied
    Evidence: .sisyphus/evidence/task-12-layout.png

  Scenario: Directory selection via Browse button
    Tool: Playwright
    Steps:
      1. Click '[data-testid="input-dir-browse"]' button
      2. Dialog opens (Electron native dialog — mock or verify IPC call sent)
      3. After selection, '[data-testid="input-dir-field"]' shows the selected path
      4. Repeat for output directory: '[data-testid="output-dir-browse"]'
    Expected Result: Both directory fields populated after selection
    Evidence: .sisyphus/evidence/task-12-browse.png

  Scenario: Scale factor auto-selects quality mode
    Tool: Playwright
    Steps:
      1. Set scale input '[data-testid="scale-input"]' to value "1.0"
      2. Assert quality dropdown '[data-testid="quality-select"]' auto-selects "DLAA"
      3. Change scale to "2.0"
      4. Assert quality dropdown auto-selects "MaxQuality"
      5. Manually select "Balanced" from quality dropdown
      6. Change scale to "3.0"
      7. Assert quality remains "Balanced" (manual override respected)
    Expected Result: Auto-quality works until manual override
    Evidence: .sisyphus/evidence/task-12-auto-quality.png

  Scenario: Interpolation enables camera data picker
    Tool: Playwright
    Steps:
      1. Assert camera data file picker '[data-testid="camera-data-picker"]' is disabled
      2. Select "2x" from interpolation dropdown '[data-testid="interpolation-select"]'
      3. Assert camera data file picker is now enabled
      4. Select "None" from interpolation dropdown
      5. Assert camera data file picker is disabled again
    Expected Result: Camera data picker toggles with interpolation selection
    Evidence: .sisyphus/evidence/task-12-interpolation.png

  Scenario: Empty directory shows validation error
    Tool: Playwright
    Steps:
      1. Clear input directory field '[data-testid="input-dir-field"]'
      2. Click elsewhere to trigger blur
      3. Assert error message visible: '[data-testid="input-dir-error"]' with text "Input directory is required"
      4. Assert input field has red border class (contains 'border-red')
    Expected Result: Validation error displayed for empty required field
    Evidence: .sisyphus/evidence/task-12-validation-error.png
  ```

  **Commit**: YES
  - Message: `feat(gui): main layout with input/output controls and basic settings`
  - Files: `gui/src/App.tsx`, `gui/src/components/InputOutputPanel.tsx`, `gui/src/components/BasicSettings.tsx`, `gui/src/components/Layout.tsx`, `gui/src/state/config-store.ts`
  - Pre-commit: `cd gui && npm run dev` (verify app launches)

- [x] 13. [F3] Advanced Settings Panel

  **What to do**:
  - Create `gui/src/components/AdvancedSettings.tsx`:
    - Collapsible section with "Advanced Settings" header + chevron toggle (collapsed by default)
    - **EXR Compression**: dropdown — None, ZIP, ZIPS, PIZ, DWAA, DWAB (default: PIZ). These are the only values supported by `ExrCompression` enum and CLI parser.
    - **DWA Quality**: number input, 0.0-500.0, step 1.0, default 45.0 (enabled only when compression is DWAA/DWAB)
    - **Output Passes**: multi-select checkboxes — Beauty (default: checked), Depth, Normals. Default: only Beauty checked (matching CLI default `OutputPass::Beauty`). Note: Depth and Normals passes are experimental/unsupported per SequenceProcessor warnings — show tooltip "(experimental)" on those checkboxes.
    - **Tonemapping**: dropdown — PQ (default), None
    - **No Inverse Tonemap**: checkbox (default unchecked)
    - **Forward LUT File**: file picker (enabled only when tonemap is not "None"), filter: `*.bin`
    - **Inverse LUT File**: file picker (enabled only when tonemap is not "None" and inverse not disabled), filter: `*.bin`
    - **Memory Budget**: slider + number input, 1-32 GB, step 1, default 8
    - **Channel Map File**: file picker, filter: `*.json`
  - Wire all advanced fields to `config-store.ts` state via dispatch
  - Show tooltips on hover for each advanced setting explaining what it does (use title or Tailwind tooltip)

  **Must NOT do**:
  - Do NOT expose dev-only flags: `--test-ngx`, `--test-vulkan`, `--test-gui`, `--test-load`, `--test-process`, `--gui`
  - Do NOT allow settings that conflict (e.g. LUT file with tonemap=None)
  - Do NOT create a second page or modal — everything inline in sidebar

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (sequential after Task 12)
  - **Blocks**: Task 15
  - **Blocked By**: Task 12

  **References**:

  **Pattern References**:
  - `gui/src/components/BasicSettings.tsx` — Follow same input component patterns (from Task 12)
  - `gui/src/state/config-store.ts` — Add actions for each new field (from Task 12)

  **API/Type References**:
  - `src/core/exr_writer.h:10-17` — `ExrCompression` enum: None, Zip, Zips, Piz, Dwaa, Dwab. GUI dropdown must expose exactly these 6 values.
  - `src/cli/cli_parser.cpp:97-122` — CLI parser for `--exr-compression` flag, accepts: `none|zip|zips|piz|dwaa|dwab`. This is the authoritative list of supported compression modes.
  - `src/cli/cli_parser.cpp:395-436` — Help text for all CLI flags including advanced ones. Tooltip text should derive from these descriptions.
  - `src/cli/config.h:41-76` — AppConfig struct fields: `exrCompression`, `exrDwaQuality`, `outputPasses`, `tonemapMode`, `noInverseTonemap`, `tonemapLutFile`, `inverseTonemapLutFile`, `memoryBudgetGB`, `channelMapFile`

  **External References**:
  - Tailwind CSS transitions (for collapsible panel): https://tailwindcss.com/docs/transition-property

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Advanced panel toggles open/closed
    Tool: Playwright
    Steps:
      1. Assert advanced panel content '[data-testid="advanced-settings-content"]' is NOT visible
      2. Click '[data-testid="advanced-settings-toggle"]'
      3. Assert advanced panel content IS visible
      4. Assert chevron rotated (class contains 'rotate-90' or 'rotate-180')
      5. Click toggle again
      6. Assert content hidden again
    Expected Result: Collapsible panel toggles visibility
    Evidence: .sisyphus/evidence/task-13-toggle.png

  Scenario: DWA quality enables only with DWAA/DWAB
    Tool: Playwright
    Steps:
      1. Open advanced settings
      2. Assert DWA quality input '[data-testid="dwa-quality-input"]' is disabled
      3. Select "DWAA" from compression dropdown '[data-testid="exr-compression-select"]'
      4. Assert DWA quality input is now enabled
      5. Set DWA quality to "85"
      6. Select "PIZ" from compression
      7. Assert DWA quality input disabled again
    Expected Result: DWA quality conditionally enabled
    Evidence: .sisyphus/evidence/task-13-dwa-conditional.png

  Scenario: Memory budget slider syncs with number input
    Tool: Playwright
    Steps:
      1. Open advanced settings
      2. Drag memory slider '[data-testid="memory-slider"]' to approximately 50% position
      3. Read number input '[data-testid="memory-input"]' value
      4. Assert value is between 14-18 (middle of 1-32 range)
      5. Set number input directly to "4"
      6. Assert slider thumb position reflects 4 GB
    Expected Result: Slider and input stay in sync
    Evidence: .sisyphus/evidence/task-13-memory-sync.png

  Scenario: LUT file pickers disabled when tonemap=None
    Tool: Playwright
    Steps:
      1. Open advanced settings
      2. Assert forward LUT picker '[data-testid="forward-lut-picker"]' is enabled (default tonemap=PQ)
      3. Select "None" from tonemap dropdown '[data-testid="tonemap-select"]'
      4. Assert forward LUT picker is disabled
      5. Assert inverse LUT picker '[data-testid="inverse-lut-picker"]' is disabled
    Expected Result: LUT pickers respect tonemap setting
    Evidence: .sisyphus/evidence/task-13-lut-disabled.png
  ```

  **Commit**: YES
  - Message: `feat(gui): advanced settings panel with conditional controls`
  - Files: `gui/src/components/AdvancedSettings.tsx`, `gui/src/state/config-store.ts` (updated)
  - Pre-commit: `cd gui && npm run dev`

- [x] 14. [F3] Processing View + Log Output + Start/Stop Controls

  **What to do**:
  - Create `gui/src/components/ProcessingView.tsx` (occupies the main right panel):
    - **Start button**: Large primary button `[data-testid="start-btn"]`, disabled when:
      - Exe path is not configured (from Task 15 settings)
      - Input directory is empty
      - Output directory is empty
      - Processing is already running
      - Camera data required but not set (interpolation != None)
      - Neither upscaling nor interpolation is enabled
    - **Stop button**: `[data-testid="stop-btn"]` Red/destructive style, visible only when processing is running
    - **Progress bar**: `[data-testid="progress-bar"]` horizontal bar showing `currentFrame / totalFrames` percentage
    - **Progress text**: `[data-testid="progress-text"]` e.g. "Processing frame 42/100 (42%)"
    - **Status indicator**: `[data-testid="status-indicator"]` — idle (gray), running (blue pulse), done (green), error (red)
    - **Log output**: `[data-testid="log-output"]` scrollable monospace textarea showing stdout/stderr lines, auto-scroll to bottom, max 5000 lines with oldest trimmed
    - **Elapsed time**: `[data-testid="elapsed-time"]` displayed while running, format "00:05:32"
  - Wire to `useProcessing` hook (from Task 11):
    - Start: calls `start(config)` with current config from store
    - Stop: calls `stop()` with confirmation dialog ("Stop processing? Current frame will be lost.")
    - Progress/log: subscribes to hook state updates
  - On completion: show toast/banner "Processing complete — N frames in HH:MM:SS"
  - On error: show error in status + last stderr lines highlighted in red in log

  **Must NOT do**:
  - Do NOT implement drag-and-drop for directories (out of scope)
  - Do NOT add queue/batch processing (single run only)
  - Do NOT add ETA calculation (elapsed time only)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (needs Task 11 hook + Task 12 layout + Task 15 exe path)
  - **Parallel Group**: Wave 3 (after Tasks 11, 12, 15)
  - **Blocks**: Task 16
  - **Blocked By**: Tasks 11, 12, 15

  **References**:

  **Pattern References**:
  - `gui/src/hooks/useProcessing.ts` — Hook API (from Task 11): `{ status, progress, currentFrame, totalFrames, log, errors, start, stop }`
  - `gui/src/components/Layout.tsx` — Main area slot where ProcessingView mounts (from Task 12)

  **API/Type References**:
  - `gui/src/state/config-store.ts` — Config context to read current config for start() call (from Task 12)
  - `gui/src/types/dlss-config.ts` — DlssConfig type (from Task 10)

  **External References**:
  - Tailwind animation utilities (for pulse): https://tailwindcss.com/docs/animation

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Start button disabled without required fields
    Tool: Playwright
    Steps:
      1. Launch app, do NOT set input/output directories
      2. Assert start button '[data-testid="start-btn"]' has attribute disabled
      3. Set input directory to a valid path
      4. Assert start button still disabled (output dir missing)
      5. Set output directory to a valid path
      6. Assert start button is now enabled
    Expected Result: Start button enables only when all required fields set
    Evidence: .sisyphus/evidence/task-14-start-disabled.png

  Scenario: Progress bar updates during processing
    Tool: Playwright
    Steps:
      1. Set valid input/output directories (can use temp dirs)
      2. Click start button '[data-testid="start-btn"]'
      3. Wait for status indicator '[data-testid="status-indicator"]' to show "running" class
      4. Assert progress bar '[data-testid="progress-bar"]' width increases over time
      5. Assert progress text '[data-testid="progress-text"]' contains "Processing frame"
      6. Assert log output '[data-testid="log-output"]' is not empty
      7. Screenshot during processing
    Expected Result: Live progress updates visible
    Failure Indicators: Progress bar stays at 0%, status never changes from idle
    Evidence: .sisyphus/evidence/task-14-progress.png

  Scenario: Stop button with confirmation dialog
    Tool: Playwright
    Steps:
      1. Start processing (with valid config)
      2. Wait for running status
      3. Click stop button '[data-testid="stop-btn"]'
      4. Assert confirmation dialog appears with text containing "Stop processing"
      5. Click "Cancel" in dialog
      6. Assert processing continues (status still "running")
      7. Click stop button again
      8. Click "Confirm" in dialog
      9. Assert status changes to "idle" or "error"
    Expected Result: Stop requires confirmation, cancel resumes
    Evidence: .sisyphus/evidence/task-14-stop-confirm.png

  Scenario: Log auto-scrolls and shows errors in red
    Tool: Playwright
    Steps:
      1. Start processing
      2. Wait for at least 5 log lines in '[data-testid="log-output"]'
      3. Assert log is scrolled to bottom (scrollTop + clientHeight ≈ scrollHeight)
      4. If stderr lines present, assert they have class containing 'text-red'
    Expected Result: Log auto-scrolls, errors highlighted
    Evidence: .sisyphus/evidence/task-14-log-scroll.png
  ```

  **Commit**: YES
  - Message: `feat(gui): processing view with progress bar, log, and start/stop`
  - Files: `gui/src/components/ProcessingView.tsx`
  - Pre-commit: `cd gui && npm run dev`

- [x] 15. [F3] Exe Path Configuration + Settings Persistence

  **What to do**:
  - Install `electron-store` in gui/:
    - `npm install electron-store` (or use `electron-store@10` for ESM compatibility if needed)
  - Create `gui/electron/settings-store.ts`:
    - Schema definition for all persisted settings:
      ```
      {
        exePath: string (default: ''),
        lastInputDir: string (default: ''),
        lastOutputDir: string (default: ''),
        scale: number (default: 2.0),
        quality: string (default: 'MaxQuality'),
        interpolation: string (default: 'None'),
        exrCompression: string (default: 'PIZ'),
        exrDwaQuality: number (default: 45.0),
        memoryBudget: number (default: 8),
        tonemapMode: string (default: 'PQ'),
        noInverseTonemap: boolean (default: false),
        windowBounds: { x, y, width, height }
      }
      ```
    - `getSettings(): Settings` — read all persisted values
    - `saveSettings(settings: Partial<Settings>): void` — merge and persist
    - `getExePath(): string` — dedicated getter for exe path
    - `setExePath(path: string): void` — dedicated setter with validation (file must end in `.exe`)
  - Create `gui/src/components/ExePathConfig.tsx`:
    - Displayed at top of sidebar, above InputOutputPanel
    - Text field showing current exe path + "Browse" button (filter: `*.exe`)
    - If path is empty: yellow warning banner "DLSS Compositor executable not configured"
    - If path is set but file not found: red error "Executable not found at path"
    - Path validated via IPC: main process checks `fs.existsSync()`
  - Wire persistence into app lifecycle:
    - On app launch: load settings → populate config store
    - On config change: debounced save (500ms) via IPC to main process
    - On window close: save window bounds
    - On window open: restore window bounds (if saved)
  - Add IPC handlers in main.ts for `getSettings` and `saveSettings`

  **Must NOT do**:
  - Do NOT auto-detect exe path from PATH or registry
  - Do NOT add exe version checking
  - Do NOT persist log output

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 13)
  - **Parallel Group**: Wave 3 (needs Task 10 IPC API + Task 12 config store)
  - **Blocks**: Task 14
  - **Blocked By**: Tasks 10, 12

  **References**:

  **Pattern References**:
  - `gui/electron/preload.ts` — IPC bridge for `getSettings()` and `saveSettings()` (from Task 10). Add new handlers following same pattern.
  - `gui/src/state/config-store.ts` — Config state shape (from Task 12). Persistence loads into this on startup.

  **API/Type References**:
  - `gui/src/types/dlss-config.ts` — DlssConfig shape defines what gets persisted (from Task 10)

  **External References**:
  - electron-store: https://github.com/sindresorhus/electron-store — Schema validation, defaults, migration API

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Exe path warning when not configured
    Tool: Playwright
    Steps:
      1. Clear electron-store data (delete config file or use fresh profile)
      2. Launch app
      3. Assert yellow warning banner '[data-testid="exe-path-warning"]' visible with text containing "not configured"
      4. Assert start button '[data-testid="start-btn"]' is disabled
    Expected Result: Clear warning when exe path missing, start disabled
    Evidence: .sisyphus/evidence/task-15-no-exe-warning.png

  Scenario: Exe path persists after restart
    Tool: Playwright
    Steps:
      1. Launch app
      2. Click browse '[data-testid="exe-path-browse"]' and select a valid .exe file
      3. Assert path displays in field '[data-testid="exe-path-field"]'
      4. Assert warning banner disappears
      5. Close and relaunch app
      6. Assert exe path field still shows the previously selected path
    Expected Result: Exe path persists across app restarts
    Evidence: .sisyphus/evidence/task-15-exe-persist.png

  Scenario: All settings persist across restart
    Tool: Playwright
    Steps:
      1. Launch app with fresh profile
      2. Set input dir to "C:\test\input"
      3. Set scale to 1.5
      4. Set quality to "DLAA"
      5. Open advanced settings, set memory budget to 4
      6. Close app (wait 600ms for debounce save)
      7. Relaunch app
      8. Assert input dir field shows "C:\test\input"
      9. Assert scale shows "1.5"
      10. Assert quality shows "DLAA"
      11. Open advanced settings, assert memory budget shows "4"
    Expected Result: All settings restored from electron-store
    Evidence: .sisyphus/evidence/task-15-settings-persist.png

  Scenario: Invalid exe path shows error
    Tool: Playwright
    Steps:
      1. Manually type "C:\nonexistent\fake.exe" into exe path field '[data-testid="exe-path-field"]'
      2. Tab out / blur
      3. Assert red error message '[data-testid="exe-path-error"]' with text containing "not found"
    Expected Result: Validation error for nonexistent exe
    Evidence: .sisyphus/evidence/task-15-exe-invalid.png
  ```

  **Commit**: YES
  - Message: `feat(gui): exe path config and electron-store persistence`
  - Files: `gui/electron/settings-store.ts`, `gui/src/components/ExePathConfig.tsx`, `gui/electron/main.ts` (updated)
  - Pre-commit: `cd gui && npm run build`

- [x] 16. [F3] Playwright Smoke Tests

  **What to do**:
  - Install Playwright as dev dependency: `npm install -D @playwright/test electron playwright`
  - Create `gui/playwright.config.ts`:
    - Use Electron launch helper (Playwright Electron API)
    - Set `testDir: './tests'`
    - Screenshot on failure to `gui/tests/screenshots/`
  - Create `gui/tests/app-launch.spec.ts`:
    - Test: App window opens with correct title
    - Test: Settings panel visible on left, main area on right
    - Test: Default settings are populated (scale=2.0, quality=MaxQuality, interpolation=None)
  - Create `gui/tests/settings-flow.spec.ts`:
    - Test: Set scale to 1.0 → quality auto-selects DLAA
    - Test: Set scale to 3.5 → quality auto-selects MaxQuality
    - Test: Open advanced settings → DWA quality disabled by default → enable DWAA → DWA quality enables
    - Test: Interpolation 2x → camera data field enables
  - Create `gui/tests/validation.spec.ts`:
    - Test: Start button disabled when no input dir
    - Test: Start button disabled when no exe path configured
    - Test: Empty input dir field shows error on blur
  - Add `"test:e2e": "playwright test"` script to gui/package.json

  **Must NOT do**:
  - Do NOT test actual DLSS processing (no GPU needed for UI tests)
  - Do NOT mock the entire Electron API — use real Electron launch via Playwright
  - Do NOT add more than 10 test cases (smoke tests, not comprehensive)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: [`playwright`]

  **Parallelization**:
  - **Can Run In Parallel**: NO (needs all UI components)
  - **Parallel Group**: Wave 4 (after Tasks 14, 15)
  - **Blocks**: Task 17
  - **Blocked By**: Tasks 14, 15

  **References**:

  **Pattern References**:
  - `gui/src/components/*.tsx` — All component data-testid attributes (from Tasks 12-14). Tests target these selectors.
  - `tests/test_cli.cpp` — Catch2 test patterns (existing). While a different framework, shows project test naming conventions.

  **External References**:
  - Playwright Electron testing: https://playwright.dev/docs/api/class-electron
  - Playwright test assertions: https://playwright.dev/docs/test-assertions

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: All Playwright tests pass
    Tool: Bash
    Steps:
      1. cd gui
      2. npx playwright test --reporter=list
      3. Capture stdout
    Expected Result: All tests pass (0 failures). Output contains "X passed" with 0 failed.
    Failure Indicators: Any test marked "failed" or "timed out"
    Evidence: .sisyphus/evidence/task-16-playwright-results.txt

  Scenario: Tests generate screenshots on failure
    Tool: Bash
    Steps:
      1. Temporarily break a test assertion (e.g., expect wrong title)
      2. Run npx playwright test --reporter=list
      3. Verify screenshot exists in gui/tests/screenshots/
      4. Revert the break
      5. Re-run tests to confirm all pass again
    Expected Result: Screenshot captured on failure, tests pass after revert
    Evidence: .sisyphus/evidence/task-16-screenshot-on-fail.png
  ```

  **Commit**: YES
  - Message: `test(gui): add Playwright smoke tests for core UI workflow`
  - Files: `gui/playwright.config.ts`, `gui/tests/*.spec.ts`, `gui/package.json` (updated)
  - Pre-commit: `cd gui && npx playwright test`

- [x] 17. [F3] UI Polish — Error States, Empty States, Validation, Edge Cases

  **What to do**:
  - **Empty states**:
    - Log area when idle: centered text "Ready to process. Configure settings and click Start." with subtle icon
    - No directory selected: placeholder text in input fields "Click Browse to select..."
  - **Error states**:
    - Processing error: Red banner at top of processing view with error message + "Dismiss" button
    - Network/IPC error: Toast notification at bottom-right, auto-dismiss after 5s
    - Invalid scale value (outside 1.0-8.0): inline error, field reset to nearest valid value on blur
  - **Validation polish**:
    - Scale input: clamp on blur (< 1.0 → 1.0, > 8.0 → 8.0), show brief flash animation on clamp
    - FPS input: clamp 1-240 on blur
    - Memory budget: clamp 1-32 on blur
    - DWA quality: clamp 0-500 on blur
    - All number inputs: reject non-numeric characters on keypress
  - **Disabled states**:
    - While processing is running: disable all settings inputs (prevent mid-run config changes)
    - All disabled inputs: `opacity-50 cursor-not-allowed` styling
  - **Keyboard navigation**:
    - Tab order follows visual layout (top-to-bottom, left-to-right)
    - Enter on Start button triggers start
    - Escape closes confirmation dialog
  - **Window behavior**:
    - Minimum window size: 1024x768 enforced in main.ts
    - Window title: "DLSS Compositor" (or with version from package.json)
  - **Loading state**:
    - On app launch: brief loading spinner while settings load from electron-store
    - Spinner centered in main area, disappears once store loaded

  **Must NOT do**:
  - Do NOT add animations beyond subtle transitions (no elaborate motion design)
  - Do NOT add sound effects or OS notifications
  - Do NOT change component structure from Tasks 12-14 — only add styling and behavior refinements
  - Do NOT add dark/light theme toggle — single dark theme only

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (final polish after all UI + tests)
  - **Parallel Group**: Wave 4 (after Task 16)
  - **Blocks**: F1-F4 (Final Verification)
  - **Blocked By**: Task 16

  **References**:

  **Pattern References**:
  - `gui/src/components/BasicSettings.tsx` — Existing input components to add validation to (from Task 12)
  - `gui/src/components/AdvancedSettings.tsx` — Existing advanced inputs to add clamping to (from Task 13)
  - `gui/src/components/ProcessingView.tsx` — Existing processing view to add empty/error states to (from Task 14)

  **API/Type References**:
  - `gui/src/state/config-store.ts` — Config state (from Task 12). Add validation logic to reducer or create validation utility.

  **External References**:
  - Tailwind transitions: https://tailwindcss.com/docs/transition-property
  - Tailwind opacity: https://tailwindcss.com/docs/opacity

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY)**:

  ```
  Scenario: Scale input clamps to valid range
    Tool: Playwright
    Steps:
      1. Set scale input '[data-testid="scale-input"]' to "0.5"
      2. Tab out (blur)
      3. Assert value is "1.0" (clamped up)
      4. Set scale input to "15"
      5. Tab out
      6. Assert value is "8.0" (clamped down)
      7. Set scale input to "abc"
      8. Assert value resets to previous valid value or 2.0 default
    Expected Result: All out-of-range values clamped, non-numeric rejected
    Evidence: .sisyphus/evidence/task-17-scale-clamp.png

  Scenario: Settings disabled during processing
    Tool: Playwright
    Steps:
      1. Configure valid settings and exe path
      2. Click start button '[data-testid="start-btn"]'
      3. Wait for status "running"
      4. Assert scale input '[data-testid="scale-input"]' has attribute disabled
      5. Assert input dir browse button '[data-testid="input-dir-browse"]' has attribute disabled
      6. Assert advanced settings toggle '[data-testid="advanced-settings-toggle"]' has attribute disabled or panel content inputs are disabled
      7. Stop processing
      8. Assert inputs re-enabled
    Expected Result: All settings locked during processing, unlocked after
    Evidence: .sisyphus/evidence/task-17-disabled-during-run.png

  Scenario: Empty state in log area
    Tool: Playwright
    Steps:
      1. Launch app fresh (no processing started)
      2. Assert log area '[data-testid="log-output"]' contains text "Ready to process"
      3. Assert no scrollbar visible (content fits)
    Expected Result: Friendly empty state message instead of blank area
    Evidence: .sisyphus/evidence/task-17-empty-state.png

  Scenario: Error banner on processing failure
    Tool: Playwright
    Steps:
      1. Set exe path to a valid executable that exits immediately with error (or use nonexistent path)
      2. Set valid input/output dirs
      3. Click start
      4. Wait for status to become "error"
      5. Assert error banner '[data-testid="error-banner"]' is visible with red background
      6. Assert banner contains error text from stderr
      7. Click dismiss button '[data-testid="error-dismiss"]'
      8. Assert banner disappears
    Expected Result: Error banner appears on failure, dismissable
    Evidence: .sisyphus/evidence/task-17-error-banner.png

  Scenario: Keyboard navigation follows visual order
    Tool: Playwright
    Steps:
      1. Focus exe path field (first focusable element)
      2. Press Tab repeatedly, record focus order
      3. Verify order: exe path → input dir → output dir → scale → quality → interpolation → start button
      4. Press Enter while start button focused → should trigger start (if valid config) or do nothing (if disabled)
      5. Press Escape while confirmation dialog open → dialog closes
    Expected Result: Tab order matches visual layout, Enter/Escape work correctly
    Evidence: .sisyphus/evidence/task-17-keyboard-nav.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): UI polish with validation, empty states, and error handling`
  - Files: `gui/src/components/*.tsx` (multiple files updated), `gui/electron/main.ts` (window constraints)
  - Pre-commit: `cd gui && npm run build; cd gui && npx playwright test`

---

## Final Verification Wave

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
>
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in .sisyphus/evidence/. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build build --config Release` + `build\tests\Release\dlss-compositor-tests.exe`. For Electron: `npm run build` in `gui/`. Review all changed files for: `as any`/`@ts-ignore`, empty catches, console.log in prod, commented-out code, unused imports. Check AI slop: excessive comments, over-abstraction, generic names.
  Output: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high` (+ `playwright` skill for Electron)
  Start from clean state. Execute EVERY QA scenario from EVERY task — follow exact steps, capture evidence. Test cross-feature: float scale in Electron GUI, Blender extension in fresh scene. Test edge cases: scale=1.0, scale=8.0, empty dirs, no GPU.
  Output: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff. Verify 1:1. Check "Must NOT do" compliance. Detect cross-task contamination. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

| # | Commit Message | Files | Pre-commit Check |
|---|---------------|-------|-----------------|
| 1 | `feat(cli): add DLAA quality mode to enum and parser` | config.h, cli_parser.cpp, ngx_wrapper.cpp | `cmake --build build` |
| 2 | `feat(cli): support float scale factor ≥1.0` | config.h, cli_parser.cpp | `cmake --build build` |
| 3 | `feat(pipeline): float scale resolution calc + routing` | sequence_processor.cpp, settings_panel.cpp | `cmake --build build` |
| 4 | `fix(pipeline): flush stdout for progress + float printf` | sequence_processor.cpp | build |
| 5 | `test(cli): add Catch2 tests for float scale and DLAA` | tests/test_cli.cpp | `dlss-compositor-tests.exe` |
| 6 | `feat(blender): convert AOV export to 5.x extension format` | blender/dlss_compositor_aov/* | blender headless test |
| 7 | `test(blender): verify extension via BlenderMCP` | .sisyphus/evidence/* | — |
| 8 | `docs: update README and usage guide for float scale + extension` | README.md, docs/usage_guide.md | — |
| 9 | `chore(gui): scaffold Electron + React + Vite + Tailwind` | gui/* | `npm run dev` |
| 10 | `feat(gui): IPC layer + child process spawning` | gui/electron/*, gui/src/ipc/* | `npm run build` |
| 11 | `feat(gui): progress parsing + stdout stream handler` | gui/src/hooks/*, gui/electron/* | build |
| 12 | `feat(gui): main layout + input/output controls` | gui/src/components/* | `npm run dev` |
| 13 | `feat(gui): advanced settings panel` | gui/src/components/* | `npm run dev` |
| 14 | `feat(gui): processing view + log + start/stop` | gui/src/components/* | `npm run dev` |
| 15 | `feat(gui): exe path config + electron-store persistence` | gui/electron/*, gui/src/* | build |
| 16 | `test(gui): Playwright smoke tests` | gui/tests/* | `npx playwright test` |
| 17 | `feat(gui): UI polish + error states + edge cases` | gui/src/components/* | build + playwright |

---

## Success Criteria

### Verification Commands
```bash
# Feature 1: Build + tests
cmake --build build --config Release    # Expected: BUILD SUCCESSFUL
build\tests\Release\dlss-compositor-tests.exe   # Expected: All tests passed

# Feature 1: CLI verification
build\Release\dlss-compositor.exe --help   # Expected: --scale shows "≥ 1.0"
build\Release\dlss-compositor.exe --scale 1.5 --quality MaxQuality --input-dir X --output-dir Y  # Expected: runs or errors on missing files (not on scale parsing)

# Feature 2: Blender headless test
"D:\Games\SteamLibrary\steamapps\common\Blender\blender.exe" --background --factory-startup --python blender\dlss_compositor_aov\__init__.py -- --test   # Expected: exit 0

# Feature 3: Electron
cd gui && npm run dev    # Expected: Electron window launches
cd gui && npm run build  # Expected: dist/ and dist-electron/ created
```

### Final Checklist
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
- [ ] All C++ tests pass
- [ ] Electron app builds and launches
- [ ] Blender extension loads in 5.x
