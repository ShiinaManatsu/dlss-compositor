# DLSS Compositor — Work Plan

## TL;DR

> **Quick Summary**: Build a standalone C++ application that reads multi-channel EXR frame sequences from Blender Cycles and processes them through NVIDIA DLSS Ray Reconstruction for offline denoising + upscaling. Includes a Blender export script and Python EXR validator.
> 
> **Deliverables**:
> - C++ compositor application (CLI + ImGui viewer) with DLSS-RR via Vulkan NGX
> - Blender Python script for one-click render pass/AOV configuration
> - Python EXR validator tool
> - English documentation (README + build guide + usage tutorial)
> 
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 4 waves
> **Critical Path**: Scaffold → Vulkan+NGX smoke test → EXR pipeline → Sequence processor → UI → Blender script → Docs

---

## Context

### Original Request
Build a toolchain for processing Blender Cycles render output through NVIDIA DLSS Ray Reconstruction. The previous agent's handoff document incorrectly stated DLSS SDK cannot be used offline — research proved this wrong. DLSS SDK v310.5.3+ supports CUDA/Vulkan APIs for arbitrary buffer processing without a game engine.

### Interview Summary
**Key Discussions**:
- Language: C++ for native NGX access
- V1 denoiser: DLSS-RR only (project differentiator, no OIDN fallback)
- V1 scope: Animation sequences only (DLSS-RR is temporal, single-frame quality is poor)
- Resolution: Render low + DLSS upscale (DLSS-RR requires input < output)
- Jitter: No Blender-controlled jitter in V1 (set to 0)
- UI: Simple ImGui + Vulkan viewer, not a node-based compositor
- Non-RTX: Error and exit with clear message
- Platform: Windows 10/11 + MSVC only for V1

**Research Findings**:
- DLSS Sample App (`DLSS_Sample_App/ngx_dlss_demo/`) demonstrates DLSS-SR, NOT DLSS-RR
- Better reference: `nvpro-samples/vk_denoise_dlssrr` (official Vulkan DLSS-RR sample)
- Buffer format mappings confirmed from `RenderTargets.hpp` and DLSS SDK headers
- Motion vector conversion needed: Blender 4ch pixel-space → DLSS-RR 2ch curr→prev
- No existing open-source tool does offline DLSS G-buffer compositing — genuine ecosystem gap

### Metis Review
**Identified Gaps** (addressed):
- DLSS-RR temporal nature: Resolved → V1 targets sequences only, `InReset=true` on first frame
- Jitter offsets: Resolved → Set to 0 for V1, accept potential quality impact
- Input < output resolution: Resolved → Standard DLSS upscale workflow
- Specular albedo mismatch: Deferred → Use raw GlossCol first, add correction if needed
- Non-RTX fallback: Resolved → Error and exit
- Platform: Resolved → Windows + MSVC only

---

## Work Objectives

### Core Objective
Create the first open-source tool for processing offline Blender renders through NVIDIA DLSS Ray Reconstruction, enabling high-quality AI denoising and upscaling of Cycles path-traced sequences.

### Concrete Deliverables
- `dlss-compositor.exe` — CLI + GUI application
- `blender/aov_export_preset.py` — Blender add-on script
- `tools/exr_validator.py` — Python validation tool
- `README.md` — Project documentation

### Definition of Done
- [ ] `cmake --build . --config Release` succeeds with zero errors on MSVC
- [ ] `dlss-compositor --test-ngx` exits 0 on RTX GPU, exits 1 with message on non-RTX
- [ ] Processing a 10-frame EXR sequence produces 10 output EXR files
- [ ] `python tools/exr_validator.py frame_0001.exr` reports PASS for all required channels
- [ ] Blender script configures all 7 required passes/AOVs when run via `blender --background`

### Must Have
- DLSS-RR processing of EXR frame sequences via Vulkan NGX API
- Multi-layer EXR reading with channel name mapping (Blender conventions)
- Motion vector conversion (Blender 4ch → DLSS-RR 2ch)
- Temporal history management (reset on first frame and sequence gaps)
- CLI batch processing mode (`--input-dir`, `--output-dir`)
- Simple ImGui viewer with channel preview and before/after comparison
- Blender script that enables all required render passes and AOVs
- Python EXR validator

### Must NOT Have (Guardrails)
- NO Donut framework dependency — raw Vulkan + volk/VMA + ImGui only
- NO OIDN, OptiX, or any non-DLSS-RR denoiser backend in V1
- NO node-based compositor UI — batch processor + preview viewer only
- NO D3D11/D3D12 backend — Vulkan only
- NO Linux/macOS support in V1
- NO real-time 3D rendering, scene loading, or camera navigation
- NO MCP server, REST API, or network functionality
- NO resolutions above 4K
- NO single-frame denoising mode (V2)
- NO modification of user Blender materials beyond adding AOV output nodes
- NO color management / OCIO integration (linear float in, linear float out)

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.

### Test Decision
- **Infrastructure exists**: NO (greenfield project)
- **Automated tests**: YES (TDD for CPU-side logic, smoke tests for GPU)
- **C++ Framework**: Catch2 (single-header)
- **Python Framework**: pytest
- **If TDD**: Phase 1 (CPU logic) follows RED→GREEN→REFACTOR. Phase 2 (GPU) uses integration smoke tests.

### QA Policy
Every task MUST include agent-executed QA scenarios.
Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **C++ modules**: Catch2 unit tests + `ctest` verification
- **GPU integration**: CLI smoke test `--test-ngx` + process test frames
- **Blender script**: `blender --background --python` headless test
- **Python tools**: pytest with fixture EXR files
- **UI**: Automated launch + channel cycling (no crash = pass)

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1a (Foundation — scaffold first):
└── Task 1: Project scaffold + CMake + Catch2 + directory structure [quick]

Wave 1b (Foundation — after Task 1, all parallel):
├── Task 2: tinyexr integration — EXR reader module + tests [quick]
├── Task 3: Channel mapper — Blender names → DLSS-RR slots + tests [quick]
├── Task 4: Motion vector converter — Blender→DLSS-RR format + tests [quick]
├── Task 5: CLI argument parser + config loader [quick]
├── Task 6: Reference test EXR fixtures (tiny 64x64) [quick]

Wave 2 (GPU integration — requires RTX GPU):
├── Task 7: Vulkan bootstrap — instance, device, queue + validation layers [deep]
├── Task 8: Vulkan texture pipeline — CPU↔GPU upload/download [deep]
├── Task 9: NGX DLSS-RR wrapper — init, create, release, shutdown [deep]
├── Task 10: NGX evaluation — feed buffers, run DLSS-RR, retrieve output [deep]
├── Task 11: Sequence processor — frame loop, temporal history, gap detection [deep]

Wave 3 (UI + Blender tooling — parallel after Wave 2 core):
├── Task 12: ImGui + Vulkan viewer — window, swapchain, ImGui context [visual-engineering]
├── Task 13: Channel preview + before/after split view [visual-engineering]
├── Task 14: Processing controls — quality mode, sequence UI [visual-engineering]
├── Task 15: Blender export script — pass config, AOV setup [unspecified-high]
├── Task 16: Python EXR validator + pytest [quick]

Wave 4 (Documentation + Polish):
├── Task 17: README + build instructions + usage guide [writing]
├── Task 18: End-to-end integration test with real EXR sequence [deep]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real manual QA (unspecified-high)
└── Task F4: Scope fidelity check (deep)
-> Present results -> Get explicit user okay

Critical Path: T1 → T7 → T9 → T10 → T11 → T18 → F1-F4 → user okay
Parallel Speedup: ~60% faster than sequential
Max Concurrent: 6 (Wave 1)
```

### Dependency Matrix

| Task | Depends On | Blocks |
|------|-----------|--------|
| 1 | — | 2,3,4,5,6,7 |
| 2 | 1 | 3,8,11,16 |
| 3 | 1,2 | 11 |
| 4 | 1 | 11 |
| 5 | 1 | 11,14 |
| 6 | 1 | 2,3,4,16 |
| 7 | 1 | 8,9,12 |
| 8 | 2,7 | 10,13 |
| 9 | 7 | 10 |
| 10 | 8,9 | 11 |
| 11 | 3,4,5,10 | 14,18 |
| 12 | 7 | 13,14 |
| 13 | 8,12 | 14 |
| 14 | 5,11,12,13 | 18 |
| 15 | — | 18 |
| 16 | 2,6 | 18 |
| 17 | — | — |
| 18 | 11,14,15,16 | F1-F4 |

### Agent Dispatch Summary

- **Wave 1**: **6 tasks** — T1-T6 → `quick`
- **Wave 2**: **5 tasks** — T7-T8 → `deep`, T9-T10 → `deep`, T11 → `deep`
- **Wave 3**: **5 tasks** — T12-T14 → `visual-engineering`, T15 → `unspecified-high`, T16 → `quick`
- **Wave 4**: **2 tasks** — T17 → `writing`, T18 → `deep`
- **FINAL**: **4 tasks** — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. Project Scaffold + CMake + Catch2 + Git Init

  **What to do**:
  - **Initialize git repository**: `git init` in project root
  - **Convert `DLSS/` to a git submodule**:
    1. Remove the existing `DLSS/` directory entirely: `rmdir /s /q DLSS`
    2. Add as submodule using the official NVIDIA URL: `git submodule add https://github.com/NVIDIA/DLSS.git DLSS`
    3. `git submodule update --init`
    4. Verify `DLSS/include/nvsdk_ngx.h` exists after submodule init
  - **Create `.gitignore`**:
    ```
    # Build
    build/
    out/
    cmake-build-*/
    *.exe
    *.obj
    *.pdb
    *.ilk
    *.lib
    *.exp

    # IDE
    .vs/
    .vscode/
    *.user
    *.suo

    # Output
    output/
    qa_output/
    test_output/
    test_gui_out/

    # Reference sample (read-only, not part of our project)
    DLSS_Sample_App/

    # Evidence (generated)
    .sisyphus/evidence/

    # Python
    __pycache__/
    *.pyc
    .pytest_cache/
    ```
  - Create directory structure: `src/core/`, `src/gpu/`, `src/dlss/`, `src/pipeline/`, `src/ui/`, `src/cli/`, `blender/`, `tools/`, `tests/`, `tests/fixtures/`, `docs/`
  - Create root `CMakeLists.txt` with C++17, MSVC, Vulkan SDK, fetch tinyexr + Catch2 + ImGui + volk + VMA via FetchContent or git submodules
  - Create empty `src/main.cpp` with placeholder `main()` that returns 0
  - Create `tests/CMakeLists.txt` with Catch2 test target
  - Create a minimal Catch2 test `tests/test_scaffold.cpp` that passes
  - Create `.gitignore` for build/, output/, DLSS_Sample_App/, IDE files, Python cache (see above)
  - Verify `cmake --build . --config Release` and `ctest` pass
  - Make initial git commit with scaffold + submodule + gitignore

  **Must NOT do**:
  - Do NOT add Donut framework or nvrhi
  - Do NOT add OpenEXR library (use tinyexr later)
  - Do NOT fetch DLSS SDK via CMake — user provides it via `DLSS_SDK_ROOT` variable

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 2-6)
  - **Blocks**: Tasks 2,3,4,5,6,7
  - **Blocked By**: None

  **References**:
  - `DLSS_Sample_App/CMakeLists.txt:1-27` — CMake structure pattern (but much simpler, no Donut)
  - `DLSS_Sample_App/ngx_dlss_demo/CMakeLists.txt:1-11` — Target setup pattern

  **Acceptance Criteria**:
  - [ ] `git status` shows clean working tree (no untracked DLSS/ or DLSS_Sample_App/ files)
  - [ ] `git submodule status` shows DLSS submodule with commit hash
  - [ ] `cmake -B build -G "Visual Studio 17 2022" -DDLSS_SDK_ROOT=DLSS` succeeds
  - [ ] `cmake --build build --config Release` produces `dlss-compositor.exe`
  - [ ] `ctest --test-dir build --config Release` runs 1 test, 0 failures

  **QA Scenarios**:
  ```
  Scenario: Build succeeds on clean checkout
    Tool: Bash
    Preconditions: MSVC 2022 and Vulkan SDK installed, CMake 3.20+
    Steps:
      1. cmake -B build -G "Visual Studio 17 2022" -DDLSS_SDK_ROOT=DLSS
      2. cmake --build build --config Release
      3. ctest --test-dir build --config Release
    Expected Result: Exit code 0 for all three commands
    Failure Indicators: Non-zero exit code, "FAILED" in ctest output
    Evidence: .sisyphus/evidence/task-1-build.txt

  Scenario: Git repo and submodule are correctly configured
    Tool: Bash
    Steps:
      1. git log --oneline -1 (initial commit exists)
      2. git submodule status (shows DLSS with commit hash, no '-' prefix)
      3. git status --porcelain (empty = clean working tree)
      4. dir DLSS\include\nvsdk_ngx.h (submodule content exists)
      5. findstr "DLSS_Sample_App" .gitignore (sample app is ignored)
    Expected Result: All commands succeed. Step 3 output is empty. Step 5 finds the entry.
    Evidence: .sisyphus/evidence/task-1-git.txt

  Scenario: Executable runs and exits cleanly
    Tool: Bash
    Preconditions: Build completed
    Steps:
      1. build\Release\dlss-compositor.exe --help
    Expected Result: Exit code 0, no crash
    Evidence: .sisyphus/evidence/task-1-run.txt
  ```

  **Commit**: YES
  - Message: `build: project scaffold with CMake, Catch2, git init, DLSS submodule`
  - Files: `CMakeLists.txt, src/main.cpp, tests/*, .gitignore, .gitmodules`
  - Pre-commit: `cmake --build build --config Release && ctest --test-dir build --config Release`

- [x] 2. tinyexr EXR Reader Module

  **What to do**:
  - Add tinyexr (single header) to the project via FetchContent or vendored copy
  - Implement `src/core/exr_reader.h` and `src/core/exr_reader.cpp`:
    - `ExrReader::open(path)` — load multi-layer EXR file
    - `ExrReader::listChannels()` — return all channel names with types
    - `ExrReader::readChannel(name)` — return float buffer for a channel
    - `ExrReader::readRGBA(r_name, g_name, b_name, a_name)` — combine 4 channels into interleaved RGBA buffer
    - `ExrReader::width()`, `ExrReader::height()` — dimensions
  - Implement `src/core/exr_writer.h` and `src/core/exr_writer.cpp`:
    - `ExrWriter::create(path, width, height)`
    - `ExrWriter::addChannel(name, data)` — add float channel
    - `ExrWriter::write()` — flush to disk
  - Write Catch2 tests in `tests/test_exr_reader.cpp`:
    - Test with reference 64×64 multi-layer EXR from `tests/fixtures/`
    - Test channel listing finds expected Blender-style names
    - Test pixel value at known coordinates
    - Test error handling for missing file, corrupt file

  **Must NOT do**:
  - Do NOT use OpenEXR library — tinyexr only
  - Do NOT handle GPU upload here — pure CPU data

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1,3-6)
  - **Blocks**: Tasks 3,8,11,16
  - **Blocked By**: Task 1 (needs CMake scaffold)

  **References**:
  - tinyexr GitHub: `https://github.com/syoyo/tinyexr` — API reference
  - Blender EXR channel naming: `{ViewLayer}.{Pass}.{Channel}` e.g. `RenderLayer.Combined.R`, `RenderLayer.Depth.Z`, `RenderLayer.Vector.X`

  **Acceptance Criteria**:
  - [ ] `ctest` passes all exr_reader tests
  - [ ] Can list all channels from reference EXR
  - [ ] Can read specific pixel values matching known reference

  **QA Scenarios**:
  ```
  Scenario: Read reference EXR channels
    Tool: Bash (ctest)
    Preconditions: Reference EXR in tests/fixtures/
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --config Release -R test_exr
    Expected Result: All tests pass (0 failures)
    Evidence: .sisyphus/evidence/task-2-exr-tests.txt

  Scenario: Error on missing file
    Tool: Bash (ctest)
    Steps:
      1. Test case attempts to open "nonexistent.exr"
    Expected Result: Returns error, does not crash or throw unhandled exception
    Evidence: .sisyphus/evidence/task-2-error-handling.txt
  ```

  **Commit**: YES
  - Message: `feat(exr): add tinyexr reader with channel extraction`
  - Files: `src/core/exr_reader.*, src/core/exr_writer.*, tests/test_exr_reader.cpp`
  - Pre-commit: `ctest --test-dir build --config Release`

- [x] 3. Channel Mapper — Blender Names to DLSS-RR Slots

  **What to do**:
  - Implement `src/core/channel_mapper.h` and `src/core/channel_mapper.cpp`:
    - Define enum `DlssBuffer { Color, Depth, MotionVectors, DiffuseAlbedo, SpecularAlbedo, Normals, Roughness }`
    - Define default Blender channel name mappings:
      ```
      Color        → RenderLayer.Combined.{R,G,B,A}
      Depth        → RenderLayer.Depth.Z
      MotionVectors→ RenderLayer.Vector.{X,Y,Z,W}
      DiffuseAlbedo→ RenderLayer.DiffCol.{R,G,B}
      SpecularAlbedo→ RenderLayer.GlossCol.{R,G,B}
      Normals      → RenderLayer.Normal.{X,Y,Z}
      Roughness    → RenderLayer.Roughness.X (custom AOV)
      ```
    - `ChannelMapper::mapFromExr(ExrReader&, config)` — extract all buffers, return struct with float data pointers
    - Support configurable name overrides via a JSON sidecar file or struct
    - Handle missing optional channels: DiffuseAlbedo defaults to white (1,1,1), SpecularAlbedo to black (0,0,0), Roughness to 0.5
    - Report which channels were found vs defaulted
  - Write Catch2 tests in `tests/test_channel_mapper.cpp`

  **Must NOT do**:
  - Do NOT do GPU upload — just CPU-side data extraction
  - Do NOT do motion vector conversion here — that's Task 4

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: Task 11
  - **Blocked By**: Tasks 1, 2

  **References**:
  - Blender docs on render passes: https://docs.blender.org/manual/en/latest/render/layers/passes.html
  - `DLSS_Sample_App/ngx_dlss_demo/glue/RenderTargets.hpp:11-22` — Buffer names pattern
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:647-665` — DLSS eval params showing required buffers

  **Acceptance Criteria**:
  - [ ] All 7 DLSS-RR buffer slots populated from reference EXR (or defaults)
  - [ ] Missing optional channels produce defaults, not crashes
  - [ ] Custom name mapping override works

  **QA Scenarios**:
  ```
  Scenario: Map all channels from standard Blender EXR
    Tool: Bash (ctest)
    Steps:
      1. ctest --test-dir build --config Release -R test_channel_mapper
    Expected Result: All 7 buffers populated, "found" status for each
    Evidence: .sisyphus/evidence/task-3-mapper-tests.txt

  Scenario: Missing optional channels get defaults
    Tool: Bash (ctest)
    Steps:
      1. Test with EXR missing Roughness and SpecularAlbedo channels
    Expected Result: Roughness defaults to 0.5, SpecularAlbedo to (0,0,0), no crash
    Evidence: .sisyphus/evidence/task-3-defaults.txt
  ```

  **Commit**: YES
  - Message: `feat(core): add Blender-to-DLSS channel name mapper`
  - Files: `src/core/channel_mapper.*, tests/test_channel_mapper.cpp`
  - Pre-commit: `ctest --test-dir build --config Release`

- [x] 4. Motion Vector Converter

  **What to do**:
  - Implement `src/core/mv_converter.h` and `src/core/mv_converter.cpp`:
    - Convert Blender Vector pass (4 channels: prev→curr X,Y + curr→next X,Y) to DLSS-RR format (2 channels: curr→prev X,Y)
    - Negate channels 0-1 to flip direction (prev→curr becomes curr→prev)
    - Flip Y axis (Blender Y-down → DLSS Y convention)
    - Output scale factors for `InMVScaleX` / `InMVScaleY` (pixel-space motion vectors, scale = 1.0 / resolution)
    - Handle edge case: zero motion vectors (static scene)
  - Write Catch2 tests in `tests/test_mv_converter.cpp`:
    - Test with synthetic data: object moved 10px right → expect (-10, 0) after conversion
    - Test Y-axis flip
    - Test zero motion vectors
    - Test scale factor computation for various resolutions

  **Must NOT do**:
  - Do NOT implement specular motion vectors (not available from Blender)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: Task 11
  - **Blocked By**: Task 1

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:659-660` — `InMVScaleX`, `InMVScaleY` usage
  - Blender docs on Vector pass: 4-channel output (Speed X/Y for previous, Speed X/Y for next)

  **Acceptance Criteria**:
  - [ ] Synthetic 10px-right motion correctly converts to (-10, 0) curr→prev
  - [ ] Y-axis flip verified with synthetic data
  - [ ] Scale factors correct for 1920×1080 and 3840×2160

  **QA Scenarios**:
  ```
  Scenario: Known motion conversion
    Tool: Bash (ctest)
    Steps:
      1. ctest --test-dir build --config Release -R test_mv_converter
    Expected Result: All tests pass
    Evidence: .sisyphus/evidence/task-4-mv-tests.txt
  ```

  **Commit**: YES
  - Message: `feat(core): add motion vector converter`
  - Files: `src/core/mv_converter.*, tests/test_mv_converter.cpp`
  - Pre-commit: `ctest --test-dir build --config Release`

- [x] 5. CLI Argument Parser + Config

  **What to do**:
  - Implement `src/cli/cli_parser.h` and `src/cli/cli_parser.cpp`:
    - Parse arguments: `--input-dir <dir>`, `--output-dir <dir>` (primary — sequence mode)
    - `--scale <factor>` (upscale factor, default 2)
    - `--quality <mode>` (MaxQuality, Balanced, Performance, UltraPerformance)
    - `--channel-map <json_file>` (optional custom channel name mapping)
    - `--encode-video [filename.mp4]` (optional: after processing, invoke FFmpeg to encode output EXR sequence to MP4/H.264. FFmpeg must be on PATH. Default filename: `output.mp4` in output dir)
    - `--fps <rate>` (frame rate for video encoding, default 24)
    - `--test-ngx` (smoke test mode: init NGX, report availability, exit)
    - `--gui` (launch ImGui viewer instead of CLI batch mode)
    - `--help` / `--version`
    - Validate input paths exist, output directory is writable
  - Implement `src/cli/config.h` — struct holding all resolved config values
  - Write Catch2 tests in `tests/test_cli.cpp`

  **Must NOT do**:
  - Do NOT implement actual processing logic here
  - Do NOT use heavy arg parsing libs — simple hand-rolled or a lightweight header-only lib

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: Tasks 11, 14
  - **Blocked By**: Task 1

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/DemoMain.cpp:1018-1056` — CommandLine parsing pattern (Windows)
  - `DLSS_Sample_App/ngx_dlss_demo/DemoMain.cpp:1058-1142` — CommandLine parsing pattern (Linux/getopt)

  **Acceptance Criteria**:
  - [ ] `--help` prints usage and exits 0
  - [ ] `--input-dir ./frames/ --output-dir ./out/` parses correctly
  - [ ] Invalid paths produce clear error message

  **QA Scenarios**:
  ```
  Scenario: Parse valid arguments
    Tool: Bash (ctest)
    Steps:
      1. ctest --test-dir build --config Release -R test_cli
    Expected Result: All tests pass
    Evidence: .sisyphus/evidence/task-5-cli-tests.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add argument parser and config`
  - Files: `src/cli/*, tests/test_cli.cpp`
  - Pre-commit: `ctest --test-dir build --config Release`

- [x] 6. Reference Test EXR Fixtures

  **What to do**:
  - Create a small Python script `tests/generate_fixtures.py` that generates:
    - `tests/fixtures/reference_64x64.exr` — Multi-layer EXR with all 7 Blender-style channels
      - Combined (RGBA): solid color gradient
      - Depth (Z): linear depth ramp
      - Vector (XYZW): synthetic motion vectors (known values)
      - Normal (XYZ): flat normals (0,0,1)
      - DiffCol (RGB): solid red (1,0,0)
      - GlossCol (RGB): solid green (0,1,0)
      - Roughness (X): constant 0.5
    - `tests/fixtures/missing_channels_64x64.exr` — EXR with only Combined + Depth (missing optional channels)
    - `tests/fixtures/sequence/frame_0001.exr` through `frame_0005.exr` — 5-frame sequence with changing motion vectors
  - Uses tinyexr or OpenEXR Python bindings to generate
  - Document pixel values at (0,0) and (31,31) for test assertions
  - Add `tests/fixtures/README.md` documenting the fixture contents

  **Must NOT do**:
  - Do NOT use real Blender renders — synthetic data with known values
  - Keep files small (64×64) for fast CI

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: Tasks 2, 3, 4, 16
  - **Blocked By**: Task 1

  **Acceptance Criteria**:
  - [ ] All fixture EXR files exist and are readable by tinyexr
  - [ ] Known pixel values at documented coordinates match expectations
  - [ ] Sequence files have incrementing motion vectors

  **QA Scenarios**:
  ```
  Scenario: Generate and validate fixtures
    Tool: Bash
    Steps:
      1. python tests/generate_fixtures.py
      2. Check file existence: tests/fixtures/reference_64x64.exr
      3. python -c "import OpenEXR; f=OpenEXR.InputFile('tests/fixtures/reference_64x64.exr'); print(f.header()['channels'])"
    Expected Result: Files created, channels listed
    Evidence: .sisyphus/evidence/task-6-fixtures.txt
  ```

  **Commit**: YES
  - Message: `test: add reference EXR fixtures`
  - Files: `tests/generate_fixtures.py, tests/fixtures/*.exr, tests/fixtures/README.md`
  - Pre-commit: `python tests/generate_fixtures.py`

- [x] 7. Vulkan Bootstrap — Instance, Device, Queue

  **What to do**:
  - Implement `src/gpu/vulkan_context.h` and `src/gpu/vulkan_context.cpp`:
    - Use volk for dynamic Vulkan loading (no static linking to vulkan-1.lib)
    - Create `VkInstance` with validation layers (debug) / without (release)
    - Enumerate physical devices, select RTX GPU that supports NGX
    - Create `VkDevice` with compute queue + optional graphics queue (for UI)
    - Create `VkCommandPool` and `VkCommandBuffer`
    - Create `VmaAllocator` (Vulkan Memory Allocator) for buffer management
    - Handle multi-GPU: prefer discrete NVIDIA GPU with Tensor Core support
    - Graceful error if no compatible GPU found (clear message, exit 1)
  - Write minimal smoke test that initializes and destroys Vulkan context

  **Must NOT do**:
  - Do NOT create window, swapchain, or any presentation logic here
  - Do NOT use nvrhi — raw Vulkan only
  - Do NOT create render passes — this is compute-only for processing

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 2, partially)
  - **Parallel Group**: Wave 2
  - **Blocks**: Tasks 8, 9, 12
  - **Blocked By**: Task 1

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:84-91` — Vulkan NGX init pattern (VkInstance, VkPhysicalDevice, VkDevice)
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:573-596` — Vulkan resource handling
  - volk: `https://github.com/zeux/volk` — dynamic Vulkan loader
  - VMA: `https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator`

  **Acceptance Criteria**:
  - [ ] Vulkan context initializes on RTX GPU
  - [ ] Clear error message on non-NVIDIA or non-RTX hardware
  - [ ] Validation layers active in debug builds

  **QA Scenarios**:
  ```
  Scenario: Vulkan initializes on RTX GPU
    Tool: Bash
    Preconditions: RTX GPU present
    Steps:
      1. dlss-compositor.exe --test-vulkan (add this debug flag)
    Expected Result: "Vulkan initialized: NVIDIA GeForce RTX XXXX", exit 0
    Evidence: .sisyphus/evidence/task-7-vulkan-init.txt

  Scenario: Graceful failure on non-RTX
    Tool: Bash
    Preconditions: No RTX GPU (or simulated)
    Steps:
      1. Attempt init with forced device index to integrated GPU
    Expected Result: "Error: No compatible NVIDIA RTX GPU found", exit 1
    Evidence: .sisyphus/evidence/task-7-no-rtx.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): Vulkan bootstrap with validation layers`
  - Files: `src/gpu/vulkan_context.*`
  - Pre-commit: `cmake --build build --config Release`

- [x] 8. Vulkan Texture Pipeline — CPU↔GPU Upload/Download

  **What to do**:
  - Implement `src/gpu/texture_pipeline.h` and `src/gpu/texture_pipeline.cpp`:
    - `uploadTexture(float* cpuData, width, height, channels, VkFormat)` → returns VkImage + VkImageView
    - `downloadTexture(VkImage, width, height, channels)` → returns float* CPU buffer
    - Support formats: `VK_FORMAT_R32_SFLOAT` (depth), `VK_FORMAT_R16G16_SFLOAT` (MV), `VK_FORMAT_R16G16B16A16_SFLOAT` (color/albedo/normals), `VK_FORMAT_R32G32B32A32_SFLOAT`
    - Use VMA staging buffers for upload/download
    - Handle format conversion: EXR float32 → VK_FORMAT_R16G16B16A16_SFLOAT (half-float)
    - Proper image layout transitions (`VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` → `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`)
    - Cleanup: destroy images, views, and free allocations
  - Connect with EXR reader: `exr_reader → float buffers → uploadTexture → VkImage`

  **Must NOT do**:
  - Do NOT handle DLSS-specific buffer semantics here — just generic upload/download

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (after Task 7)
  - **Parallel Group**: Wave 2
  - **Blocks**: Tasks 10, 13
  - **Blocked By**: Tasks 2, 7

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/glue/RenderTargets.hpp:38-106` — Texture creation patterns and format choices
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:574-596` — `TextureToResourceVK()` converting textures to NGX resources
  - VMA documentation: `https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/`

  **Acceptance Criteria**:
  - [ ] Can upload a 64×64 float buffer and download it back with matching values
  - [ ] Round-trip test: upload → download → compare with epsilon tolerance
  - [ ] All Vulkan objects properly destroyed (no validation layer errors)

  **QA Scenarios**:
  ```
  Scenario: Round-trip upload/download
    Tool: Bash (ctest)
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --config Release -R test_texture_pipeline
    Expected Result: Test "roundtrip_upload_download" passes — uploads 64x64 float buffer to VkImage, downloads back, asserts pixel values match within float16 epsilon (1e-3). Exit code 0.
    Failure Indicators: ctest reports FAIL, pixel mismatch exceeds epsilon
    Evidence: .sisyphus/evidence/task-8-roundtrip.txt

  Scenario: No Vulkan validation errors
    Tool: Bash
    Steps:
      1. set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
      2. ctest --test-dir build --config Release -R test_texture_pipeline 2>&1 | findstr /i "validation error"
    Expected Result: No lines containing "validation error" in stderr. Exit code 0 from ctest.
    Failure Indicators: Any "validation error" or "validation warning" in output
    Evidence: .sisyphus/evidence/task-8-validation.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): CPU-GPU texture upload/download pipeline`
  - Files: `src/gpu/texture_pipeline.*`
  - Pre-commit: `cmake --build build --config Release`

- [x] 9. NGX DLSS-RR Wrapper — Init, Create, Release, Shutdown

  **What to do**:
  - Implement `src/dlss/ngx_wrapper.h` and `src/dlss/ngx_wrapper.cpp`:
    - `NgxContext::init(VkInstance, VkPhysicalDevice, VkDevice)` — Initialize NGX
    - `NgxContext::isDlssRRAvailable()` — Check if DLSS-RR feature is supported
    - `NgxContext::createDlssRR(inputWidth, inputHeight, outputWidth, outputHeight, qualityMode)` — Create DLSS-RR feature
    - `NgxContext::releaseDlssRR()` — Release feature
    - `NgxContext::shutdown()` — Shutdown NGX
    - Use `NVSDK_NGX_Feature_RayReconstruction` (Feature ID 14), NOT SuperSampling
    - Use `NVSDK_NGX_VULKAN_Init`, `NVSDK_NGX_VULKAN_GetCapabilityParameters`
    - Use `NGX_VULKAN_CREATE_DLSSD_EXT1` (NOT `NGX_VULKAN_CREATE_DLSS_EXT` — note the "1" suffix and "D" for Denoiser)
    - Set `InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked` (separate roughness channel)
    - Set `InUseHWDepth = 0` (we provide linear depth, not hardware depth buffer)
    - Error handling: log NGX error codes with `GetNGXResultAsString()`
  - Implement `--test-ngx` CLI mode: init NGX, check availability, print result, cleanup, exit

  **Must NOT do**:
  - Do NOT implement DLSS-SR or Frame Generation
  - Do NOT implement frame evaluation here — just lifecycle management
  - Do NOT hard-code APP_ID — use configurable or 0 for development

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 8, after Task 7)
  - **Parallel Group**: Wave 2
  - **Blocks**: Task 10
  - **Blocked By**: Task 7

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.h:56-116` — NGXWrapper class interface (adapt for DLSS-RR)
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:65-216` — NGX init/shutdown lifecycle
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:308-408` — Feature creation (DLSS-SR version — adapt for DLSS-RR)
  - DLSS SDK (git submodule at `DLSS/`): `DLSS/include/nvsdk_ngx_helpers_dlssd_vk.h` — DLSS-RR Vulkan helper macros
  - DLSS SDK: `DLSS/include/nvsdk_ngx_defs_dlssd.h` — DLSS-RR parameter names and enums
  - Reference sample: `nvpro-samples/vk_denoise_dlssrr` — Official DLSS-RR Vulkan sample (clone and study)
  - DLSS-RR Integration Guide: `DLSS/doc/DLSS-RR Integration Guide.pdf`

  **Acceptance Criteria**:
  - [ ] `dlss-compositor --test-ngx` prints "DLSS-RR available: true" on RTX GPU
  - [ ] `dlss-compositor --test-ngx` prints "DLSS-RR not available" + reason on non-RTX, exits 1
  - [ ] NGX initializes and shuts down without memory leaks or validation errors

  **QA Scenarios**:
  ```
  Scenario: NGX smoke test on RTX
    Tool: Bash
    Preconditions: RTX GPU with compatible driver
    Steps:
      1. dlss-compositor.exe --test-ngx
    Expected Result: stdout contains "DLSS-RR available: true", exit code 0
    Evidence: .sisyphus/evidence/task-9-ngx-smoke.txt

  Scenario: NGX smoke test on non-RTX
    Tool: Bash
    Preconditions: No RTX GPU
    Steps:
      1. dlss-compositor.exe --test-ngx
    Expected Result: stderr contains "requires NVIDIA RTX", exit code 1
    Evidence: .sisyphus/evidence/task-9-no-rtx.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): NGX DLSS-RR wrapper`
  - Files: `src/dlss/ngx_wrapper.*`
  - Pre-commit: `cmake --build build --config Release && build\Release\dlss-compositor.exe --test-ngx`

- [x] 10. NGX Evaluation — Feed Buffers, Run DLSS-RR, Retrieve Output

  **What to do**:
  - Implement `src/dlss/dlss_rr_processor.h` and `src/dlss/dlss_rr_processor.cpp`:
    - `DlssRRProcessor::evaluate(DlssFrameInput&)` — process a single frame
    - `DlssFrameInput` struct:
      ```cpp
      struct DlssFrameInput {
          VkImage color;           // Noisy rendered color
          VkImage depth;           // Linear depth
          VkImage motionVectors;   // Converted curr→prev MV
          VkImage diffuseAlbedo;   // Diffuse albedo
          VkImage specularAlbedo;  // Specular albedo
          VkImage normals;         // World-space normals
          VkImage roughness;       // Surface roughness
          VkImage output;          // Output (upscaled resolution)
          float jitterX, jitterY;  // Sub-pixel jitter (0 for V1)
          float mvScaleX, mvScaleY;// Motion vector scale
          bool reset;              // Reset temporal history
      };
      ```
    - Populate `NVSDK_NGX_VK_DLSSD_Eval_Params` from DlssFrameInput
    - Wrap VkImages as `NVSDK_NGX_Resource_VK` using `NVSDK_NGX_Create_ImageView_Resource_VK`
    - Call `NGX_VULKAN_EVALUATE_DLSSD_EXT`
    - Handle errors and return success/failure
  - Smoke test: create a Catch2 test `test_dlss_rr_processor` that uploads reference EXR buffers, calls `evaluate()` once with `reset=true`, downloads output, verifies output is non-zero. This test runs INDEPENDENTLY of the sequence processor (Task 11).

  **Must NOT do**:
  - Do NOT implement sequence logic (temporal history across frames) — that's Task 11
  - Do NOT implement UI display — just process and save

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (sequential after Tasks 8, 9)
  - **Parallel Group**: Wave 2 (sequential)
  - **Blocks**: Task 11
  - **Blocked By**: Tasks 8, 9

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:598-774` — EvaluateSuperSampling (adapt Vulkan section for DLSS-RR)
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:574-596` — TextureToResourceVK conversion function
  - `DLSS_Sample_App/ngx_dlss_demo/NGXWrapper.cpp:740-758` — Vulkan DLSS eval params population
  - DLSS SDK (git submodule at `DLSS/`): `DLSS/include/nvsdk_ngx_helpers_dlssd_vk.h` — `NGX_VULKAN_CREATE_DLSSD_EXT1` (line 129), `NGX_VULKAN_EVALUATE_DLSSD_EXT` (line 155)

  **Acceptance Criteria**:
  - [ ] `ctest -R test_dlss_rr_processor` passes on RTX GPU (SKIP on non-RTX)
  - [ ] Output buffer has correct upscaled dimensions (input × scale)
  - [ ] Output pixels are non-zero (DLSS-RR actually produced output)

  **QA Scenarios**:
  ```
  Scenario: DLSS-RR single evaluation smoke test (task-local, no sequence processor)
    Tool: Bash (ctest)
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --config Release -R test_dlss_rr_processor
    Expected Result: Test reads reference EXR fixture, uploads all 7 buffers, calls DlssRRProcessor::evaluate() with reset=true, downloads output, asserts output dimensions = 128x128 (64x64 input × 2 scale) and at least one pixel != 0. On non-RTX: test reports SKIP (not FAIL). Exit code 0.
    Failure Indicators: ctest FAIL, output all zeros, NGX error code in log
    Evidence: .sisyphus/evidence/task-10-smoke.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): DLSS-RR evaluation with buffer feeding`
  - Files: `src/dlss/dlss_rr_processor.*`
  - Pre-commit: `cmake --build build --config Release`

- [x] 11. Sequence Processor — Frame Loop, Temporal History, Gap Detection

  **What to do**:
  - Implement `src/pipeline/sequence_processor.h` and `src/pipeline/sequence_processor.cpp`:
    - `SequenceProcessor::processDirectory(inputDir, outputDir, config)` — main entry point
    - Scan input directory for EXR files, sort by name (natural numeric order)
    - Detect frame number gaps (e.g., frame_0001, frame_0003 → gap at frame_0002)
    - For first frame in each contiguous run: set `reset=true`
    - For subsequent frames: set `reset=false` (DLSS-RR accumulates temporal history)
    - For each frame:
      1. Read EXR via ExrReader
      2. Map channels via ChannelMapper
      3. Convert motion vectors via MvConverter
      4. Upload all buffers to GPU via TexturePipeline
      5. Evaluate DLSS-RR via DlssRRProcessor
      6. Download output from GPU
      7. Write output EXR via ExrWriter
      8. Free GPU resources for current frame (keep DLSS-RR internal history)
    - Progress reporting to stdout: `Processing frame N/M: filename.exr`
    - Error handling: if one frame fails, log error and continue to next (don't abort entire sequence)
    - After all frames processed, if `--encode-video` was specified:
      1. Check FFmpeg is on PATH (`where ffmpeg`)
      2. Invoke FFmpeg to encode output EXR sequence to MP4/H.264: `ffmpeg -y -framerate {fps} -i {output_dir}/frame_%04d.exr -c:v libx264 -pix_fmt yuv420p -crf 18 {output.mp4}`
      3. Log success/failure of encoding (non-fatal: if FFmpeg not found, warn but exit 0)

  **Must NOT do**:
  - Do NOT load all frames into memory — stream one at a time
  - Do NOT implement parallel frame processing (DLSS-RR is sequential for temporal)

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (needs all Wave 2 predecessors)
  - **Parallel Group**: Wave 2 (final)
  - **Blocks**: Tasks 14, 18
  - **Blocked By**: Tasks 3, 4, 5, 10

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/DemoMain.cpp:626-632` — AdvanceFrame + temporal state management
  - `DLSS_Sample_App/ngx_dlss_demo/DemoMain.cpp:944` — Reset flag logic (`!m_PreviousViewsValid`)

  **Acceptance Criteria**:
  - [ ] 5-frame fixture sequence produces 5 output files
  - [ ] First frame has `reset=true`, subsequent frames `reset=false`
  - [ ] Frame gaps detected and logged
  - [ ] Missing frame doesn't crash — error logged, processing continues

  **QA Scenarios**:
  ```
  Scenario: Process 5-frame sequence
    Tool: Bash
    Steps:
      1. dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir test_out/ --scale 2
    Expected Result: 5 output files in test_out/, stdout shows progress "1/5 ... 5/5", exit 0
    Evidence: .sisyphus/evidence/task-11-sequence.txt

  Scenario: Gap detection
    Tool: Bash
    Steps:
      1. Delete frame_0003.exr from test sequence
      2. Process sequence
    Expected Result: Warning logged about gap, temporal reset at frame_0004, output files still created
    Evidence: .sisyphus/evidence/task-11-gaps.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): sequence processor with temporal history`
  - Files: `src/pipeline/sequence_processor.*`
  - Pre-commit: `cmake --build build --config Release`

- [x] 12. ImGui + Vulkan Viewer Scaffold

  **What to do**:
  - Implement `src/ui/app.h` and `src/ui/app.cpp`:
    - Create GLFW window (1280×720 default)
    - Create Vulkan swapchain for presentation
    - Initialize Dear ImGui with `ImGui_ImplGlfw_*` + `ImGui_ImplVulkan_*`
    - Main loop: poll events → new frame → render ImGui → present
    - Clean shutdown of all resources
    - `--test-gui` flag: initialize everything, render 5 frames to offscreen, then exit 0 (no window shown, no human interaction)
    - Window title: "DLSS Compositor v0.1"
  - Wire into `main.cpp`: launch GUI when `--gui` flag is passed
  - Fetch ImGui + GLFW via FetchContent in CMakeLists.txt

  **Must NOT do**:
  - Do NOT display images yet — just empty ImGui window
  - Do NOT implement any processing controls — just scaffold

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3 (with Tasks 13-16)
  - **Blocks**: Tasks 13, 14
  - **Blocked By**: Task 7

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/glue/UIRenderer.hpp:1-18` — ImGui renderer setup pattern
  - ImGui Vulkan example: `https://github.com/ocornut/imgui/blob/master/examples/example_glfw_vulkan/main.cpp`

  **Acceptance Criteria**:
  - [ ] `dlss-compositor --gui` opens a window with ImGui rendered
  - [ ] Window closes cleanly on X button
  - [ ] No Vulkan validation errors

  **QA Scenarios**:
  ```
  Scenario: GUI init/shutdown smoke test (non-interactive)
    Tool: Bash
    Steps:
      1. dlss-compositor.exe --test-gui
    Expected Result: App initializes Vulkan+ImGui, renders 5 offscreen frames, exits with code 0. No window shown, no human interaction needed.
    Failure Indicators: Non-zero exit code, crash, Vulkan validation errors in stderr
    Evidence: .sisyphus/evidence/task-12-gui-launch.txt
  ```

  **Commit**: YES
  - Message: `feat(ui): ImGui+Vulkan viewer scaffold`
  - Files: `src/ui/app.*`
  - Pre-commit: `cmake --build build --config Release && build\Release\dlss-compositor.exe --test-gui`

- [x] 13. Channel Preview + Before/After Split View

  **What to do**:
  - Implement `src/ui/image_viewer.h` and `src/ui/image_viewer.cpp`:
    - Load EXR file → upload all channels to GPU textures
    - ImGui dropdown to select channel (Color, Depth, Normal, MV, DiffAlbedo, SpecAlbedo, Roughness)
    - Display selected channel as a full-viewport texture
    - Depth visualization: remap to 0-1 range with configurable near/far
    - Motion vector visualization: encode XY as RG color
    - Before/After split view: drag-able vertical divider, left=noisy input, right=denoised output
    - Zoom: scroll wheel, fit-to-window button
    - Info bar: resolution, channel name, pixel value under cursor

  **Must NOT do**:
  - Do NOT implement processing — just viewing existing EXR data and pre-computed outputs

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 14, after Tasks 8, 12)
  - **Parallel Group**: Wave 3
  - **Blocks**: Task 14
  - **Blocked By**: Tasks 8, 12

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/glue/UIRenderer.hpp:46-57` — ImGui text/combo UI pattern
  - `DLSS_Sample_App/ngx_dlss_demo/glue/UIRenderer.hpp:67-131` — Combo dropdowns for mode selection

  **Acceptance Criteria**:
  - [ ] All 7 channels display without crash
  - [ ] Depth and MV channels have proper visualization (not raw floats)
  - [ ] Split view divider is draggable

  **QA Scenarios**:
  ```
  Scenario: Channel switching smoke test (non-interactive)
    Tool: Bash
    Steps:
      1. dlss-compositor.exe --test-gui --test-load tests/fixtures/reference_64x64.exr
    Expected Result: App loads EXR, creates GPU textures for all 7 channels, renders each to offscreen, exits 0. No window shown.
    Failure Indicators: Non-zero exit code, segfault, "failed to create texture" in stderr
    Evidence: .sisyphus/evidence/task-13-channels.txt
  ```

  **Commit**: YES
  - Message: `feat(ui): channel preview and split view`
  - Files: `src/ui/image_viewer.*`
  - Pre-commit: `cmake --build build --config Release && build\Release\dlss-compositor.exe --test-gui --test-load tests/fixtures/reference_64x64.exr`

- [x] 14. Processing Controls — Quality Mode, Sequence UI

  **What to do**:
  - Implement `src/ui/settings_panel.h` and `src/ui/settings_panel.cpp`:
    - Input directory selector (file dialog or path text input)
    - Output directory selector
    - Scale factor dropdown (2×, 3×, 4×)
    - Quality mode dropdown (MaxQuality, Balanced, Performance, UltraPerformance)
    - Channel name mapping editor (show detected → expected mapping, allow overrides)
    - "Process Sequence" button → triggers SequenceProcessor in background thread
    - Progress bar during processing
    - Frame navigation: prev/next buttons for viewing individual frames
    - Status bar: "Ready", "Processing frame N/M", "Complete", "Error: ..."
  - Wire settings to CLI config struct so GUI and CLI share the same config

  **Must NOT do**:
  - Do NOT block UI during processing — use std::thread or similar
  - Do NOT add timeline/scrubbing/playback

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (needs 5, 11, 12, 13)
  - **Parallel Group**: Wave 3 (final)
  - **Blocks**: Task 18
  - **Blocked By**: Tasks 5, 11, 12, 13

  **References**:
  - `DLSS_Sample_App/ngx_dlss_demo/glue/UIRenderer.hpp:68-177` — Complete ImGui settings panel pattern

  **Acceptance Criteria**:
  - [ ] Can select directories via text input
  - [ ] Process button triggers processing with progress bar
  - [ ] Quality mode selection works

  **QA Scenarios**:
  ```
  Scenario: Settings + processing smoke test (non-interactive)
    Tool: Bash
    Steps:
      1. dlss-compositor.exe --test-gui --test-process --input-dir tests/fixtures/sequence/ --output-dir test_gui_out/ --scale 2
    Expected Result: App initializes GUI subsystem, processes sequence via SequenceProcessor in background thread, waits for completion, verifies output file count matches input, exits 0. No window shown.
    Failure Indicators: Non-zero exit code, output file count mismatch, deadlock (timeout >60s)
    Evidence: .sisyphus/evidence/task-14-gui-process.txt
  ```

  **Commit**: YES
  - Message: `feat(ui): processing controls and settings`
  - Files: `src/ui/settings_panel.*`
  - Pre-commit: `cmake --build build --config Release`

- [x] 15. Blender Export Script — Pass Config, AOV Setup

  **What to do**:
  - Implement `blender/aov_export_preset.py` as a proper Blender add-on:
    - `bl_info` dict with name, version, blender version, category
    - Operator `DLSSCOMP_OT_configure_passes`:
      1. Enable built-in render passes: Combined, Z (Depth), Vector, Normal, DiffCol, GlossCol
      2. Add custom AOV "Roughness" (type: VALUE) to the active view layer
      3. For each material in the scene with a Principled BSDF:
         - Add AOV Output node connected to Roughness input → "Roughness" AOV
         - Skip materials that already have the AOV Output node
      4. Configure compositor: add File Output node set to OpenEXR MultiLayer, RAW, ZIP compression
      5. Connect all render passes to File Output node inputs
    - Panel `DLSSCOMP_PT_export_panel` in Render Properties:
      - Output directory path selector
      - "Configure All Passes" button (runs operator)
      - Status: show which passes are enabled/missing
    - `register()` / `unregister()` functions
    - Works in Blender 4.2+ (current stable) through 5.1+
    - Testable headless: `blender --background --python blender/aov_export_preset.py -- --test`

  **Must NOT do**:
  - Do NOT render — only configure passes, user clicks render themselves
  - Do NOT modify material shading beyond adding AOV output nodes
  - Do NOT control camera jitter
  - Do NOT add i18n (V1 is English only)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (independent of C++ tasks)
  - **Parallel Group**: Wave 3
  - **Blocks**: Task 18
  - **Blocked By**: None

  **References**:
  - Blender Python API: `https://docs.blender.org/api/current/`
  - Blender render passes: `bpy.context.view_layer.use_pass_z`, `use_pass_vector`, `use_pass_normal`, etc.
  - Custom AOV: `bpy.context.view_layer.aovs.add()` with name="Roughness"

  **Acceptance Criteria**:
  - [ ] `blender --background --python blender/aov_export_preset.py -- --test` exits 0
  - [ ] All 6 built-in passes enabled after running operator
  - [ ] Roughness AOV created on view layer
  - [ ] Compositor File Output node configured

  **QA Scenarios**:
  ```
  Scenario: Headless pass configuration test
    Tool: Bash
    Steps:
      1. blender --background --factory-startup --python blender/aov_export_preset.py -- --test
    Expected Result: Exit 0, stdout shows "Passes configured: Combined, Z, Vector, Normal, DiffCol, GlossCol, Roughness(AOV)"
    Evidence: .sisyphus/evidence/task-15-blender-test.txt

  Scenario: AOV node added to materials
    Tool: Bash
    Steps:
      1. blender --background --python blender/aov_export_preset.py -- --test-materials
    Expected Result: Every Principled BSDF material has AOV Output node for Roughness
    Evidence: .sisyphus/evidence/task-15-aov-nodes.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): AOV export preset script`
  - Files: `blender/aov_export_preset.py`
  - Pre-commit: `blender --background --factory-startup --python blender/aov_export_preset.py -- --test`

- [x] 16. Python EXR Validator + pytest

  **What to do**:
  - Implement `tools/exr_validator.py`:
    - CLI: `python exr_validator.py <exr_file_or_dir> [--strict]`
    - List all channels in the EXR file
    - Check for required channels (with Blender naming convention):
      - Required: Combined (RGBA), Depth (Z), Vector (XYZW), Normal (XYZ)
      - Optional: DiffCol (RGB), GlossCol (RGB), Roughness (X)
    - Report PASS/FAIL per channel with format info (float16/float32)
    - Check resolution consistency within multi-layer EXR
    - `--strict` mode: also check optional channels
    - Directory mode: validate all .exr files, report summary
    - Use `OpenEXR` Python package (pip installable)
    - Exit code: 0 = all pass, 1 = any fail
  - Create `requirements.txt` with `OpenEXR` dependency
  - Write pytest tests in `tests/test_validator.py`:
    - Test with reference EXR (expect PASS)
    - Test with missing-channels EXR (expect FAIL in strict, PASS in normal)
    - Test with non-existent file (expect error message, exit 1)

  **Must NOT do**:
  - Do NOT validate pixel data quality — just structural validation
  - Do NOT depend on tinyexr — use Python OpenEXR package

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: Task 18
  - **Blocked By**: Tasks 2, 6 (needs fixtures for tests)

  **References**:
  - Python OpenEXR: `https://pypi.org/project/OpenEXR/`

  **Acceptance Criteria**:
  - [ ] `python tools/exr_validator.py tests/fixtures/reference_64x64.exr` exits 0, reports PASS
  - [ ] `python tools/exr_validator.py tests/fixtures/missing_channels_64x64.exr --strict` exits 1
  - [ ] `pytest tests/test_validator.py` passes all tests

  **QA Scenarios**:
  ```
  Scenario: Validate good EXR
    Tool: Bash
    Steps:
      1. python tools/exr_validator.py tests/fixtures/reference_64x64.exr
    Expected Result: "PASS" for all required channels, exit 0
    Evidence: .sisyphus/evidence/task-16-validator-pass.txt

  Scenario: Detect missing channels
    Tool: Bash
    Steps:
      1. python tools/exr_validator.py tests/fixtures/missing_channels_64x64.exr --strict
    Expected Result: "FAIL" for missing channels, exit 1
    Evidence: .sisyphus/evidence/task-16-validator-fail.txt
  ```

  **Commit**: YES
  - Message: `feat(tools): EXR validator with pytest`
  - Files: `tools/exr_validator.py, requirements.txt, tests/test_validator.py`
  - Pre-commit: `pytest tests/test_validator.py`

- [x] 17. Documentation — README + Build Guide + Usage Tutorial

  **What to do**:
  - Write `README.md`:
    - Project description: what it does, why it exists
    - Features list
    - Requirements: NVIDIA RTX GPU, Windows 10/11, Vulkan SDK, DLSS SDK, CMake, MSVC
    - Quick start: build → configure Blender → export → process → view
    - Screenshots placeholder
    - License
  - Write `docs/build_guide.md`:
    - Prerequisites installation (Vulkan SDK, DLSS SDK download)
    - CMake configuration with `DLSS_SDK_ROOT`
    - Build steps
    - Troubleshooting (common build errors)
  - Write `docs/usage_guide.md`:
    - Blender scene setup and script usage
    - CLI usage with all flags documented
    - GUI walkthrough
    - Expected output format
    - Troubleshooting (NGX errors, driver issues)
  - Write `docs/dlss_input_spec.md`:
    - DLSS-RR input buffer specification
    - Blender channel → DLSS buffer mapping table
    - Motion vector conversion details
    - Known limitations

  **Must NOT do**:
  - Do NOT write in Chinese (V1 English only)
  - Do NOT add badges, CI config, or deployment docs

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (can start anytime)
  - **Parallel Group**: Wave 4
  - **Blocks**: None
  - **Blocked By**: None (but should reference final API)

  **Acceptance Criteria**:
  - [ ] README contains working build instructions
  - [ ] All CLI flags documented
  - [ ] Blender workflow documented step-by-step

  **QA Scenarios**:
  ```
  Scenario: Build instructions match actual build flow
    Tool: Bash
    Steps:
      1. cmake -B build_verify -G "Visual Studio 17 2022" -DDLSS_SDK_ROOT=DLSS
      2. cmake --build build_verify --config Release
      3. ctest --test-dir build_verify --config Release
      4. build_verify\Release\dlss-compositor.exe --help
    Expected Result: All 4 commands succeed with exit code 0. --help output matches documented CLI flags in README.
    Failure Indicators: Any command fails, --help output missing documented flags
    Evidence: .sisyphus/evidence/task-17-docs-build.txt

  Scenario: CLI flags documented in README match implementation
    Tool: Bash
    Steps:
      1. build_verify\Release\dlss-compositor.exe --help > help_output.txt
      2. findstr "input-dir" help_output.txt
      3. findstr "output-dir" help_output.txt
      4. findstr "scale" help_output.txt
      5. findstr "encode-video" help_output.txt
      6. findstr "test-ngx" help_output.txt
    Expected Result: All findstr commands find matches (exit 0)
    Evidence: .sisyphus/evidence/task-17-flags-check.txt
  ```

  **Commit**: YES
  - Message: `docs: README, build guide, usage tutorial`
  - Files: `README.md, docs/*.md`

- [x] 18. End-to-End Integration Test

  **What to do**:
  - Create `tests/test_integration.cpp` (Catch2):
    - Full pipeline test: read fixture EXR → map channels → convert MV → upload → DLSS-RR → download → write output
    - Verify output file exists, has correct resolution (input × scale), is valid EXR
    - Sequence test: process 5-frame fixture sequence, verify 5 output files
    - Skip gracefully if no RTX GPU (Catch2 SKIP, not FAIL)
  - Create `tests/test_e2e.py` (pytest):
    - Run `dlss-compositor` CLI as subprocess with fixture data
    - Verify exit code, output file count, output file sizes
    - Run validator on output files
  - Update CMakeLists.txt to include integration tests as separate ctest target

  **Must NOT do**:
  - Do NOT require real Blender renders — use 64×64 fixtures
  - Do NOT make GPU tests fail on non-RTX — use SKIP

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on everything)
  - **Parallel Group**: Wave 4
  - **Blocks**: F1-F4
  - **Blocked By**: Tasks 11, 14, 15, 16

  **References**:
  - All previous task outputs and interfaces

  **Acceptance Criteria**:
  - [ ] `ctest --test-dir build --config Release -R integration` passes on RTX GPU
  - [ ] `pytest tests/test_e2e.py` passes
  - [ ] Non-RTX systems skip GPU tests (not fail)

  **QA Scenarios**:
  ```
  Scenario: Full pipeline integration on RTX
    Tool: Bash
    Steps:
      1. ctest --test-dir build --config Release -R integration
      2. pytest tests/test_e2e.py
    Expected Result: All tests pass (or skip on non-RTX)
    Evidence: .sisyphus/evidence/task-18-e2e.txt
  ```

  **Commit**: YES
  - Message: `test: end-to-end integration test`
  - Files: `tests/test_integration.cpp, tests/test_e2e.py`
  - Pre-commit: `ctest --test-dir build --config Release`

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in .sisyphus/evidence/. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

  **QA Scenarios**:
  ```
  Scenario: Must Have compliance
    Tool: Bash
    Steps:
      1. dir /b src\dlss\ngx_wrapper.* (DLSS-RR wrapper exists)
      2. dir /b src\core\mv_converter.* (Motion vector converter exists)
      3. dir /b src\core\channel_mapper.* (Channel mapper exists)
      4. dir /b src\pipeline\sequence_processor.* (Sequence processor exists)
      5. dir /b src\ui\app.* (ImGui viewer exists)
      6. dir /b blender\aov_export_preset.py (Blender script exists)
      7. dir /b tools\exr_validator.py (Validator exists)
      8. dir /b README.md (Documentation exists)
      9. build\Release\dlss-compositor.exe --test-ngx (NGX smoke test runs)
    Expected Result: All dir commands find files (exit 0). --test-ngx exits 0 on RTX.
    Evidence: .sisyphus/evidence/F1-must-have.txt

  Scenario: Must NOT Have compliance
    Tool: Bash + Grep
    Steps:
      1. grep -r "donut" src/ --include="*.cpp" --include="*.h" -l (no Donut dependency)
      2. grep -r "nvrhi" src/ --include="*.cpp" --include="*.h" -l (no nvrhi dependency)
      3. grep -r "OIDN\|oidn\|OpenImageDenoise" src/ --include="*.cpp" --include="*.h" -l (no OIDN)
      4. grep -r "D3D11\|D3D12\|d3d11\|d3d12" src/ --include="*.cpp" --include="*.h" -l (no D3D)
    Expected Result: All grep commands return no matches (exit 1 = no match found).
    Evidence: .sisyphus/evidence/F1-must-not-have.txt
  ```

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build . --config Release` + Catch2 tests + pytest. Review all changed files for: undefined behavior, memory leaks, missing error checks, `#pragma warning(disable)` abuse. Check for AI slop: excessive comments, over-abstraction, dead code.
  Output: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: Build and test suite
    Tool: Bash
    Steps:
      1. cmake --build build --config Release 2>&1 | findstr /i "error"
      2. ctest --test-dir build --config Release --output-on-failure
      3. pytest tests/ -v
    Expected Result: Step 1 produces no error lines. Step 2 reports 0 failures. Step 3 reports all passed.
    Evidence: .sisyphus/evidence/F2-build-tests.txt

  Scenario: Code smell scan
    Tool: Grep
    Steps:
      1. grep -rn "as any\|@ts-ignore\|#pragma warning(disable" src/ --include="*.cpp" --include="*.h"
      2. grep -rn "TODO\|FIXME\|HACK\|XXX" src/ --include="*.cpp" --include="*.h"
      3. grep -rn "console.log\|printf.*debug\|std::cout.*debug" src/ --include="*.cpp" --include="*.h"
    Expected Result: Step 1 returns zero matches. Steps 2-3 return zero or minimal matches (each reviewed).
    Evidence: .sisyphus/evidence/F2-code-smells.txt
  ```

- [x] F3. **Real Manual QA** — `unspecified-high`
  Start from clean state. Build project. Run `dlss-compositor --test-ngx`. Process a test EXR sequence. Launch GUI in test mode. Run validator on output. Save to `.sisyphus/evidence/final-qa/`.
  Output: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: End-to-end pipeline
    Tool: Bash
    Steps:
      1. cmake -B build_qa -G "Visual Studio 17 2022" -DDLSS_SDK_ROOT=DLSS
      2. cmake --build build_qa --config Release
      3. build_qa\Release\dlss-compositor.exe --test-ngx
      4. build_qa\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir qa_output/ --scale 2
      5. python tools/exr_validator.py qa_output/ 
      6. build_qa\Release\dlss-compositor.exe --test-gui --test-load qa_output/frame_0001.exr
      7. dir /b qa_output\*.exr | find /c /v "" 
    Expected Result: Steps 1-6 exit 0. Step 7 outputs "5" (matching 5 input frames).
    Evidence: .sisyphus/evidence/F3-e2e.txt

  Scenario: Blender script headless test
    Tool: Bash
    Steps:
      1. blender --background --factory-startup --python blender/aov_export_preset.py -- --test
    Expected Result: Exit 0, stdout contains "Passes configured"
    Evidence: .sisyphus/evidence/F3-blender.txt
  ```

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff. Verify 1:1 — everything in spec was built, nothing beyond spec was built. Check "Must NOT do" compliance. Detect cross-task contamination. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: Diff audit per task
    Tool: Bash
    Steps:
      1. git log --oneline --all (list all commits, should match 18 planned commits)
      2. git diff --stat HEAD~18..HEAD (total changed files across all commits)
      3. dir /s /b src\*.cpp src\*.h | find /c /v "" (count source files — should be ~20-30)
    Expected Result: Step 1 shows 18+ commits matching planned commit messages. Step 3 shows reasonable file count (no unexplained extra files).
    Evidence: .sisyphus/evidence/F4-scope.txt

  Scenario: No out-of-scope files
    Tool: Bash + Grep
    Steps:
      1. dir /s /b *.cmake *.json *.xml | findstr /v "build node_modules .git" (check for unexpected config files)
      2. git status --porcelain (no untracked/modified files after final commit)
    Expected Result: Step 1 shows only expected CMake/JSON files. Step 2 is empty (clean working tree).
    Evidence: .sisyphus/evidence/F4-clean.txt
  ```

---

## Commit Strategy

| Phase | Commit | Message | Key Files |
|-------|--------|---------|-----------|
| 1 | 1 | `build: project scaffold with CMake, Catch2, git init, DLSS submodule` | CMakeLists.txt, .gitignore, .gitmodules, src/main.cpp, tests/ |
| 1 | 2 | `feat(exr): add tinyexr reader with channel extraction` | src/core/exr_reader.*, tests/test_exr_reader.cpp |
| 1 | 3 | `feat(core): add Blender-to-DLSS channel name mapper` | src/core/channel_mapper.*, tests/test_channel_mapper.cpp |
| 1 | 4 | `feat(core): add motion vector converter` | src/core/mv_converter.*, tests/test_mv_converter.cpp |
| 1 | 5 | `feat(cli): add argument parser and config` | src/cli/*, tests/test_cli.cpp |
| 1 | 6 | `test: add reference EXR fixtures` | tests/fixtures/*.exr |
| 2 | 7 | `feat(gpu): Vulkan bootstrap with validation layers` | src/gpu/vulkan_context.* |
| 2 | 8 | `feat(gpu): CPU-GPU texture upload/download pipeline` | src/gpu/texture_pipeline.* |
| 2 | 9 | `feat(dlss): NGX DLSS-RR wrapper` | src/dlss/ngx_wrapper.* |
| 2 | 10 | `feat(dlss): DLSS-RR evaluation with buffer feeding` | src/dlss/dlss_rr_processor.* |
| 2 | 11 | `feat(pipeline): sequence processor with temporal history` | src/pipeline/sequence_processor.* |
| 3 | 12 | `feat(ui): ImGui+Vulkan viewer scaffold` | src/ui/app.* |
| 3 | 13 | `feat(ui): channel preview and split view` | src/ui/image_viewer.* |
| 3 | 14 | `feat(ui): processing controls and settings` | src/ui/settings_panel.* |
| 3 | 15 | `feat(blender): AOV export preset script` | blender/aov_export_preset.py |
| 3 | 16 | `feat(tools): EXR validator with pytest` | tools/exr_validator.py, tests/test_validator.py |
| 4 | 17 | `docs: README, build guide, usage tutorial` | README.md, docs/ |
| 4 | 18 | `test: end-to-end integration test` | tests/test_integration.cpp |

---

## Success Criteria

### Verification Commands
```bash
cmake --build . --config Release        # Expected: BUILD SUCCEEDED
ctest --config Release                  # Expected: All tests passed
dlss-compositor --test-ngx              # Expected: "DLSS-RR available: true", exit 0
dlss-compositor --input-dir ./frames/ --output-dir ./out/ --scale 2  # Expected: exit 0, output files created
python tools/exr_validator.py frames/frame_0001.exr  # Expected: "PASS: All required channels present"
blender --background --python blender/aov_export_preset.py  # Expected: exit 0, passes configured
```

### Final Checklist
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
- [ ] C++ builds with zero errors on MSVC
- [ ] Catch2 tests pass
- [ ] pytest passes
- [ ] CLI processes EXR sequence end-to-end
- [ ] ImGui viewer launches and displays channels without crash
- [ ] Blender script configures all passes in headless mode
- [ ] README contains build instructions that actually work
