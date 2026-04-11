# Combined RR→FG Pipeline & Performance Optimization

## TL;DR

> **Quick Summary**: Add a combined DLSS-RR + DLSS-FG processing mode that upscales then interpolates in a single GPU pass (no intermediate download), and implement three performance optimization layers (batch uploads, texture pooling, async EXR prefetch) across all processing paths.
>
> **Deliverables**:
> - Combined RR→FG pipeline (`processDirectoryRRFG()`) activated when both `--scale` and `--interpolate` are set
> - `TexturePipeline::uploadBatch()` method eliminating per-texture GPU sync points
> - `TexturePool` class for cross-frame VkImage reuse
> - `FramePrefetcher` class for background EXR reading
> - Updated CLI help, main.cpp availability checks, integration tests, docs
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 4 waves
> **Critical Path**: T1 → T2 → T3 → T4

---

## Context

### Original Request
Implement two features for the DLSS Compositor project:
1. **Combined RR→FG Pipeline** — Allow `--scale` and `--interpolate` to work together, chaining DLSS-RR upscaling into DLSS-FG interpolation with the RR output staying on GPU.
2. **Performance Optimization** — Three layers: batch GPU uploads, texture pool for VkImage reuse, async EXR prefetch with background threading.

### Codebase Architecture
- `src/pipeline/sequence_processor.cpp` (976 lines) — Main processing logic. `processDirectory()` at line 368 has a 2-way branch: `interpolateFactor > 0` routes to `processDirectoryFG()` (line 662), else runs RR path inline.
- `src/gpu/texture_pipeline.cpp` (508 lines) — `upload()` at line 94 creates a staging buffer, VkImage, transitions layout, copies, and transitions back. Each upload is a separate `beginOneTimeCommands()`/`endOneTimeCommands()` cycle (lines 185/210), which means a separate `vkQueueSubmit` + `vkQueueWaitIdle`. **8 uploads per RR frame = 8 GPU pipeline stalls.**
- `src/cli/cli_parser.cpp` (360 lines) — Parses CLI flags. `--scale` at line 205, `--interpolate` at line 218. Post-parse validation at line 324 requires `--camera-data` with `--interpolate`.
- `src/main.cpp` (136 lines) — DLSS-RR availability check at line 113 unconditionally blocks execution if RR is unavailable, even for FG-only mode.
- `src/dlss/ngx_wrapper.h` — `createDlssRR()` takes input/output dimensions + quality; `createDlssFG()` takes width/height/format.
- `TextureHandle` struct (texture_pipeline.h:15): `VkImage`, `VkImageView`, `VmaAllocation`, `VkFormat`, width, height, channels.
- `MappedBuffers` struct (channel_mapper.h:41): vectors of float for color/depth/motionVectors/diffuseAlbedo/specularAlbedo/normals/roughness.
- Tests: Catch2 framework, `FakeArgs` pattern for CLI tests, GPU tests SKIP when no RTX GPU available.
- Build: CMake, sources explicitly listed in `CMakeLists.txt:111-122` and `tests/CMakeLists.txt:2-14`.

### Key Technical Facts (from Streamline SDK / NGX API)
- FG receives the **upscaled backbuffer** from RR output — the RR output TextureHandle can be passed directly as FG backbuffer input
- **Depth and motion vectors stay at render resolution** for FG — NOT upscaled
- **mvecScale stays the same** (`{1/renderWidth, 1/renderHeight}`) for both RR and FG
- Camera matrices (clipToPrevClip) are in **NDC space** — resolution-independent, no adjustment needed
- DlssFG feature must be **created at the upscaled resolution** (`outputWidth`, `outputHeight`)
- RR output image layout must be transitioned from SHADER_READ_ONLY to GENERAL before FG can use it as backbuffer

---

## Work Objectives

### Core Objective
Enable combined RR→FG processing and significantly reduce per-frame GPU overhead through batched uploads, texture reuse, and async I/O.

### Concrete Deliverables
- `processDirectoryRRFG()` method in `sequence_processor.cpp`
- 3-way branch in `processDirectory()` at line 374
- Fixed `main.cpp` DLSS-RR availability check (FG-only must work without RR)
- `TexturePipeline::uploadBatch()` method
- `TexturePool` class (`src/gpu/texture_pool.h/.cpp`)
- `FramePrefetcher` class (`src/pipeline/frame_prefetcher.h/.cpp`)
- `--memory-budget` CLI flag
- Updated CLI help text for combined mode
- Integration test for combined RR+FG mode
- CLI tests for combined flags and `--memory-budget`
- Updated README.md and docs/usage_guide.md

### Definition of Done
- [ ] `cmake --build build --config Release` succeeds with zero errors
- [ ] `build\tests\Release\dlss-compositor-tests.exe` — all 45+ existing tests pass
- [ ] New tests for combined mode, batch upload, texture pool, prefetcher, CLI flags all pass
- [ ] Combined mode works: `--input-dir X --output-dir Y --scale 2 --interpolate 2x --camera-data Z` produces correct frame count and upscaled resolution
- [ ] FG-only mode still works without DLSS-RR GPU capability

### Must Have
- 3-way branch logic in processDirectory()
- RR output stays on GPU (no download+re-upload between RR and FG)
- Depth/MVs stay at render resolution for FG input
- DlssFG created at upscaled resolution
- Both DlssFeatureGuard and DlssFgFeatureGuard for RAII cleanup
- Reset flag handling for both RR and FG features
- uploadBatch() uses single command buffer + single vkQueueSubmit
- TexturePool reuses VkImage allocations across frames
- FramePrefetcher uses background thread with ring buffer
- All new code is C++17 compatible (no C++20 features)

### Must NOT Have (Guardrails)
- NO modifications to `DlssRRProcessor` or `DlssFgProcessor` classes
- NO modifications to `DlssFrameInput` or `DlssFgFrameInput` structs
- NO C++20 features (no `std::jthread`, `std::span`, concepts, ranges, `std::format`)
- NO downloading RR output to CPU and re-uploading for FG — data stays on GPU
- NO upscaling depth or motion vectors for FG input
- NO changing mvecScale when feeding into FG (stays at `{1/renderWidth, 1/renderHeight}`)
- NO modifying existing `processDirectory()` RR-only path logic (only add the branch at line 374)
- NO modifying existing `processDirectoryFG()` logic (only the dispatch branch)
- NO global state or singletons for TexturePool/FramePrefetcher
- NO busy-waiting in FramePrefetcher (use condition_variable)

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.

### Test Decision
- **Infrastructure exists**: YES (Catch2, tests/ directory, 45 existing tests)
- **Automated tests**: TDD where practical (CLI tests, unit tests for new classes); tests-alongside for GPU integration
- **Framework**: Catch2
- **Build command**: `cmake --build build --config Release`
- **Test command**: `build\tests\Release\dlss-compositor-tests.exe`

### QA Policy
Every task includes agent-executed QA scenarios. Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **Build verification**: `cmake --build build --config Release` — zero errors
- **Test verification**: `build\tests\Release\dlss-compositor-tests.exe` — all pass, including new tests
- **CLI verification**: Run executable with `--help` to verify updated text
- **Integration**: Run with combined flags on test fixtures (requires RTX GPU — SKIP if unavailable)

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately — independent foundations):
├── T1: CLI changes (allow combined flags, --memory-budget, help text) [quick]
├── T5: TexturePipeline::uploadBatch() method [deep]
├── T8: TexturePool class [deep]
└── T12: FramePrefetcher class [deep]

Wave 2 (After Wave 1 — core integration):
├── T2: processDirectoryRRFG() implementation + main.cpp fix (depends: T1) [deep]
├── T6: Integrate batch upload into processing functions (depends: T5) [unspecified-high]
├── T9: Integrate TexturePool into processing functions (depends: T8) [unspecified-high]
└── T13: Integrate FramePrefetcher into processing functions (depends: T12) [unspecified-high]

Wave 3 (After Wave 2 — tests):
├── T3: Integration test for combined RR+FG mode (depends: T2) [unspecified-high]
├── T7: Tests for uploadBatch (depends: T6) [unspecified-high]
├── T10: Tests for TexturePool (depends: T9) [unspecified-high]
└── T14: Tests for FramePrefetcher (depends: T13) [unspecified-high]

Wave 4 (After Wave 3 — docs):
├── T4: README.md + docs/usage_guide.md update (depends: T2, T3) [quick]
└── T11: CLI test for --memory-budget (depends: T9) [quick]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── F1: Plan compliance audit (oracle)
├── F2: Code quality review (unspecified-high)
├── F3: Real manual QA (unspecified-high)
└── F4: Scope fidelity check (deep)
→ Present results → Get explicit user okay

Critical Path: T1 → T2 → T3 → T4 → FINAL
Parallel Speedup: ~60% faster than sequential
Max Concurrent: 4 (Wave 1)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| T1   | —         | T2, T11 | 1 |
| T5   | —         | T6     | 1 |
| T8   | —         | T9     | 1 |
| T12  | —         | T13    | 1 |
| T2   | T1        | T3, T4 | 2 |
| T6   | T5        | T7     | 2 |
| T9   | T8        | T10, T11 | 2 |
| T13  | T12       | T14    | 2 |
| T3   | T2        | T4     | 3 |
| T7   | T6        | —      | 3 |
| T10  | T9        | —      | 3 |
| T14  | T13       | —      | 3 |
| T4   | T2, T3    | —      | 4 |
| T11  | T1, T9    | —      | 4 |

### Agent Dispatch Summary

- **Wave 1**: 4 tasks — T1 → `quick`, T5 → `deep`, T8 → `deep`, T12 → `deep`
- **Wave 2**: 4 tasks — T2 → `deep`, T6 → `unspecified-high`, T9 → `unspecified-high`, T13 → `unspecified-high`
- **Wave 3**: 4 tasks — T3 → `unspecified-high`, T7 → `unspecified-high`, T10 → `unspecified-high`, T14 → `unspecified-high`
- **Wave 4**: 2 tasks — T4 → `quick`, T11 → `quick`
- **FINAL**: 4 tasks — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [ ] 1. CLI Changes — Allow Combined --scale + --interpolate Flags

  **What to do**:
  - In `src/cli/cli_parser.cpp`: No mutual exclusion to remove (currently there is none — `--scale` and `--interpolate` are parsed independently). Verify this is still true after reading the full parser.
  - In `src/cli/cli_parser.cpp` `printHelp()` (line 332-356): Add a line after the `--interpolate` entry explaining combined mode:
    ```
    "  --memory-budget <GB>   GPU memory budget for texture pool (default: 8, min: 1)\n"
    ```
    And update the `--scale` / `--interpolate` descriptions to mention they can be combined:
    ```
    "  --scale <factor>       Upscale factor (2, 3, or 4). Can combine with --interpolate.\n"
    "  --interpolate <mode>   Frame interpolation (2x or 4x; requires --camera-data). Can combine with --scale.\n"
    ```
  - In `src/cli/config.h`: Add `int memoryBudgetGB = 8;` field to `AppConfig` struct (after line 43, the `cameraDataFile` field).
  - In `src/cli/cli_parser.cpp`: Add `--memory-budget` flag parsing (after the `--camera-data` block at line 237):
    ```cpp
    if (std::strcmp(arg, "--memory-budget") == 0) {
        if (i + 1 >= argc) {
            errorMsg = "--memory-budget requires a value";
            return false;
        }
        int budget = 0;
        if (!parseInt(argv[++i], budget) || budget < 1) {
            errorMsg = "--memory-budget must be an integer >= 1";
            return false;
        }
        config.memoryBudgetGB = budget;
        continue;
    }
    ```
  - In `src/main.cpp` (line 113-118): Fix the DLSS-RR availability check. Currently:
    ```cpp
    if (!ngx.isDlssRRAvailable()) {
        fprintf(stderr, "DLSS-RR not available: %s\n", ngx.unavailableReason().c_str());
        ngx.shutdown();
        context.destroy();
        return 1;
    }
    ```
    Change to only block if RR is actually needed (i.e., `scaleFactor > 1` or no interpolation):
    ```cpp
    if (config.interpolateFactor == 0 || config.scaleFactor > 1) {
        if (!ngx.isDlssRRAvailable()) {
            fprintf(stderr, "DLSS-RR not available: %s\n", ngx.unavailableReason().c_str());
            ngx.shutdown();
            context.destroy();
            return 1;
        }
    }
    ```
    This allows FG-only mode (`--interpolate 2x` without `--scale` or with `--scale 1`) to proceed when DLSS-RR is not available.
  - In `tests/test_cli.cpp`: Add test cases:
    - "CliParser - combined --scale and --interpolate" — verifies both flags parse together
    - "CliParser - --memory-budget valid" — verifies `memoryBudgetGB` is set
    - "CliParser - --memory-budget invalid" — verifies error on `--memory-budget 0`
    - "CliParser - --memory-budget missing value" — verifies error when no value given

  **Must NOT do**:
  - Do NOT add any mutual exclusion between --scale and --interpolate
  - Do NOT change the default `scaleFactor` value (currently 2)
  - Do NOT modify any existing test cases

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Small, well-scoped changes across 4 files — CLI parsing, config struct, help text, simple tests
  - **Skills**: []
    - No special skills needed for simple C++ edits

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T5, T8, T12)
  - **Blocks**: T2 (needs the config field and main.cpp fix), T11 (needs --memory-budget parsing)
  - **Blocked By**: None — can start immediately

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:218-229` — `--interpolate` parsing pattern (follow exact same style for `--memory-budget`)
  - `src/cli/cli_parser.cpp:205-216` — `--scale` parsing pattern with parseInt and validation
  - `src/cli/cli_parser.cpp:332-356` — `printHelp()` format (follow exact printf style)
  - `tests/test_cli.cpp:8-15` — `FakeArgs` struct pattern for CLI test argv simulation
  - `tests/test_cli.cpp:111-128` — `--interpolate` test pattern (follow same structure for combined and --memory-budget tests)

  **API/Type References**:
  - `src/cli/config.h:35-63` — `AppConfig` struct (add `memoryBudgetGB` field here)
  - `src/dlss/ngx_wrapper.h:30-31` — `isDlssRRAvailable()` and `unavailableReason()` signatures

  **External References**: None needed

  **WHY Each Reference Matters**:
  - `cli_parser.cpp:218-229` — Exact pattern for how to parse a flag with a value, validate it, and set config. Copy this structure.
  - `config.h:35-63` — Where to add the new field. Must be after `cameraDataFile` to maintain logical grouping.
  - `main.cpp:113-118` — The critical bug: this blocks FG-only mode. The fix must make the check conditional.
  - `test_cli.cpp:8-15` — FakeArgs is how ALL CLI tests work. Use this for new tests.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Combined flags parse successfully
    Tool: Bash (build + test)
    Preconditions: Clean build
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe "CliParser - combined --scale and --interpolate"
      3. Assert test passes
    Expected Result: Test PASS — cfg.scaleFactor == 2, cfg.interpolateFactor == 2
    Failure Indicators: Test fails, compile error, or assertion failure
    Evidence: .sisyphus/evidence/task-1-combined-flags-parse.txt

  Scenario: --memory-budget parses valid value
    Tool: Bash (build + test)
    Preconditions: Clean build
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "CliParser - --memory-budget valid"
      2. Assert test passes
    Expected Result: Test PASS — cfg.memoryBudgetGB == 4
    Failure Indicators: Test fails or compile error
    Evidence: .sisyphus/evidence/task-1-memory-budget-parse.txt

  Scenario: --memory-budget rejects invalid value
    Tool: Bash (build + test)
    Preconditions: Clean build
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "CliParser - --memory-budget invalid"
      2. Assert test passes
    Expected Result: Test PASS — parse returns false, error message non-empty
    Failure Indicators: Test fails (parse succeeds when it shouldn't)
    Evidence: .sisyphus/evidence/task-1-memory-budget-invalid.txt

  Scenario: Help text mentions combined mode
    Tool: Bash
    Preconditions: Successful build
    Steps:
      1. build\Release\dlss-compositor.exe --help
      2. Verify output contains "Can combine with --interpolate" (in --scale line)
      3. Verify output contains "--memory-budget"
    Expected Result: Both strings present in help output
    Failure Indicators: Help text doesn't include new text
    Evidence: .sisyphus/evidence/task-1-help-text.txt

  Scenario: All existing 45 tests still pass
    Tool: Bash
    Preconditions: Successful build
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify "All tests passed" or 0 failures in output
    Expected Result: All existing tests pass (no regressions)
    Failure Indicators: Any test failure
    Evidence: .sisyphus/evidence/task-1-existing-tests.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): allow combined --scale + --interpolate flags and add --memory-budget`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`, `src/main.cpp`, `tests/test_cli.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 2. processDirectoryRRFG() — Combined RR→FG Processing Mode

  **What to do**:
  - In `src/pipeline/sequence_processor.h`: Add `processDirectoryRRFG()` as a private method (after `processDirectoryFG` declaration at line 35):
    ```cpp
    bool processDirectoryRRFG(const std::string& inputDir,
                              const std::string& outputDir,
                              const AppConfig& config,
                              std::string& errorMsg);
    ```
  - In `src/pipeline/sequence_processor.cpp` at line 374: Replace the 2-way branch with a 3-way branch:
    ```cpp
    if (config.interpolateFactor > 0 && config.scaleFactor > 1) {
        return processDirectoryRRFG(inputDir, outputDir, config, errorMsg);
    }
    if (config.interpolateFactor > 0) {
        return processDirectoryFG(inputDir, outputDir, config, errorMsg);
    }
    ```
    The existing RR-only code below continues as-is.
  - In `src/pipeline/sequence_processor.cpp`: Implement `processDirectoryRRFG()`. This is the core feature. The function must:

    **Setup phase** (modeled after processDirectoryFG lines 662-754):
    1. Call `scanAndSort()`, validate ≥2 frames, create output directory, check paths not equivalent
    2. Load camera data via `CameraDataLoader`, validate all frames have camera entries
    3. Determine `multiFrameCount` from `config.interpolateFactor` (1 for 2x, 3 for 4x)
    4. Check `m_ngx.isDlssFGAvailable()` — return error if not
    5. Check `m_ngx.isDlssRRAvailable()` — return error if not (RR is needed for combined mode)
    6. Check 4x multi-frame support via `m_ngx.maxMultiFrameCount()`
    7. Open first frame to get render resolution (`expectedInputWidth`, `expectedInputHeight`)
    8. Compute output resolution: `expectedOutputWidth = expectedInputWidth * config.scaleFactor`, `expectedOutputHeight = expectedInputHeight * config.scaleFactor`
    9. Create DLSS-RR feature: `m_ngx.createDlssRR(expectedInputWidth, expectedInputHeight, expectedOutputWidth, expectedOutputHeight, config.quality, createCmdBuf, errorMsg)` — modeled after line 447-453
    10. Create DLSS-FG feature: `m_ngx.createDlssFG(expectedOutputWidth, expectedOutputHeight, VK_FORMAT_R16G16B16A16_SFLOAT, createCmdBuf, errorMsg)` — at upscaled resolution! Modeled after line 741-745
    11. Create BOTH guards: `DlssFeatureGuard featureGuardRR(m_ngx);` and `DlssFgFeatureGuard featureGuardFG(m_ngx);`
    12. Create processor objects: `DlssRRProcessor rrProcessor(m_ctx, m_ngx);` and `DlssFgProcessor fgProcessor(m_ctx, m_ngx);`

    **Frame 0** (first frame — RR only, no interpolation):
    1. Read first frame EXR, map channels via `ChannelMapper`
    2. Convert motion vectors via `MvConverter::convert()`
    3. Expand RGB to RGBA for diffuse/specular/normals
    4. Upload 8 RR textures at render resolution + output at upscaled resolution (following pattern from lines 500-539)
    5. Run `rrProcessor.evaluate()` (following pattern from lines 566-591)
    6. Download RR output, write as `frame_0001` using `writeRgbaExr()` (following pattern from lines 593-617)
    7. Destroy all texture handles

    **Frame pairs loop** (i=1..N-1):
    For each frame pair (previousFrame, currentFrame):
    1. Load camera data for the pair: `cameraLoader.computePairParams()` (following line 824-829)
    2. Read current frame EXR, map channels, convert MVs, expand RGB→RGBA
    3. Compute reset flags (gap detection, same pattern as line 786-795)

    **RR pass** (within the frame pair loop):
    4. Upload 8 RR textures at render resolution + RR output texture at upscaled resolution
    5. Build `DlssFrameInput` struct (following lines 541-564)
    6. Set `frame.mvScaleX = mvResult.scaleX; frame.mvScaleY = mvResult.scaleY;` (render-resolution scale)
    7. Set `frame.reset` based on reset flag
    8. Transition RR output image to GENERAL, evaluate RR, transition back to SHADER_READ_ONLY
    9. Submit and wait

    **FG pass** (within the frame pair loop, after RR):
    10. The RR output TextureHandle is the FG backbuffer — pass `rrOutput.image` / `rrOutput.view` directly as `fgInput.backbuffer` / `fgInput.backbufferView`. **NO download+re-upload.**
    11. Depth and motion textures: reuse the SAME TextureHandles from the RR step. They are already at render resolution on GPU. Pass `depth.image`/`depth.view` and `motion.image`/`motion.view` to FG.
    12. FG dimensions: `fgInput.width = expectedOutputWidth; fgInput.height = expectedOutputHeight;` — upscaled resolution
    13. Camera params, multiFrameCount, multiFrameIndex — same as processDirectoryFG pattern (line 871-885)
    14. For each `multiFrameIndex` (1..multiFrameCount):
        a. Create outputInterp texture at upscaled resolution
        b. Transition outputInterp to GENERAL
        c. Build `DlssFgFrameInput` struct
        d. Evaluate FG: `fgProcessor.evaluate(evalCmdBuf, fgInput, errorMsg)`
        e. Transition outputInterp back to SHADER_READ_ONLY
        f. Submit and wait
        g. Download outputInterp, write as next output frame
        h. Destroy outputInterp handle
    15. After FG loop: download RR output, write as original upscaled frame (same as frame 0 pattern)
    16. Destroy all per-frame texture handles (color, depth, motion, diffuse, specular, normals, roughness, rrOutput)

    **Cleanup / post-loop**:
    17. Handle `config.encodeVideo` (same pattern as lines 643-657 / 959-973)
    18. Both guards handle cleanup automatically via RAII destructors

    **Output frame numbering**: Same as processDirectoryFG — `outputFrameCounter` starts at 1, increments for each written frame. Frame 0 = original upscaled, then for each pair: interpolated frames first, then original upscaled.

    **Critical technical details**:
    - **mvecScale stays the same** for both RR and FG: `{mvResult.scaleX, mvResult.scaleY}` which is `{1/renderWidth, 1/renderHeight}`. FG does NOT need the scale adjusted for the upscaled resolution. The motion vectors describe movement at render resolution and FG handles the resolution mismatch internally.
    - **Camera matrices are resolution-independent** (NDC space). No adjustment needed for upscaled resolution.
    - **Layout transitions**: RR output must be transitioned from SHADER_READ_ONLY (after RR evaluate) to GENERAL (for FG to read as backbuffer), then potentially back. Follow the same `transitionImage()` helper used throughout the file.

  **Must NOT do**:
  - Do NOT modify `DlssRRProcessor`, `DlssFgProcessor`, `DlssFrameInput`, `DlssFgFrameInput`
  - Do NOT modify the existing RR-only path (lines 378-660) beyond adding the branch at line 374
  - Do NOT modify `processDirectoryFG()` (lines 662-976)
  - Do NOT download RR output and re-upload for FG
  - Do NOT upscale depth or motion vectors
  - Do NOT change mvecScale for FG input

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Complex GPU pipeline logic requiring careful Vulkan resource management, layout transitions, and precise data flow between two DLSS features
  - **Skills**: []
    - No special skills needed — deep understanding of codebase patterns from references suffices

  **Parallelization**:
  - **Can Run In Parallel**: NO (within Wave 2)
  - **Parallel Group**: Wave 2 (with T6, T9, T13)
  - **Blocks**: T3 (integration test), T4 (docs)
  - **Blocked By**: T1 (needs config changes and main.cpp fix)

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:368-376` — Current 2-way branch to replace with 3-way
  - `src/pipeline/sequence_processor.cpp:390-636` — RR-only processing loop (pattern for frame iteration, ExrReader, ChannelMapper, MvConverter, texture upload, evaluate, download, write)
  - `src/pipeline/sequence_processor.cpp:662-976` — FG processing loop (pattern for frame pairs, camera data, multiFrameIndex loop, FG evaluate, output naming)
  - `src/pipeline/sequence_processor.cpp:500-539` — 8 RR texture uploads (exact upload calls to replicate)
  - `src/pipeline/sequence_processor.cpp:541-564` — DlssFrameInput struct population
  - `src/pipeline/sequence_processor.cpp:566-591` — RR evaluate with layout transitions
  - `src/pipeline/sequence_processor.cpp:593-617` — RR output download and EXR write
  - `src/pipeline/sequence_processor.cpp:840-933` — FG texture uploads, DlssFgFrameInput population, FG evaluate loop
  - `src/pipeline/sequence_processor.cpp:31-49` — DlssFeatureGuard and DlssFgFeatureGuard RAII structs
  - `src/pipeline/sequence_processor.cpp:625-632` — destroyHandle cleanup pattern

  **API/Type References**:
  - `src/dlss/dlss_rr_processor.h:14-40` — `DlssFrameInput` struct (all fields, DO NOT modify)
  - `src/dlss/dlss_rr_processor.h:42-50` — `DlssRRProcessor::evaluate()` signature
  - `src/dlss/dlss_fg_processor.h:16-40` — `DlssFgFrameInput` struct (all fields, DO NOT modify)
  - `src/dlss/dlss_fg_processor.h:42-50` — `DlssFgProcessor::evaluate()` signature
  - `src/dlss/ngx_wrapper.h:33-39` — `createDlssRR()` signature (inputW, inputH, outputW, outputH, quality, cmdBuf)
  - `src/dlss/ngx_wrapper.h:43-47` — `createDlssFG()` signature (width, height, format, cmdBuf)
  - `src/gpu/texture_pipeline.h:15-23` — `TextureHandle` struct
  - `src/core/channel_mapper.h:41-57` — `MappedBuffers` struct
  - `src/pipeline/sequence_processor.h:12-15` — `SequenceFrameInfo` struct

  **WHY Each Reference Matters**:
  - Lines 500-539 / 541-564 — Exact texture upload calls and DlssFrameInput population to replicate in the new function
  - Lines 840-933 — FG loop pattern to replicate (multiFrameIndex, outputInterp creation, evaluate, download)
  - Lines 31-49 — RAII guards must be used for both features; shows the pattern
  - ngx_wrapper.h:43-47 — createDlssFG takes (width, height, format, cmdBuf) — must pass UPSCALED dimensions
  - dlss_fg_processor.h:16-40 — DlssFgFrameInput.backbuffer is where RR output goes; width/height must be upscaled resolution

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with new function
    Tool: Bash
    Preconditions: T1 committed
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build with processDirectoryRRFG() compiled
    Failure Indicators: Compile errors, linker errors
    Evidence: .sisyphus/evidence/task-2-build.txt

  Scenario: All existing tests still pass
    Tool: Bash
    Preconditions: Successful build
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All 45+ existing tests pass (no regressions)
    Failure Indicators: Any test failure
    Evidence: .sisyphus/evidence/task-2-existing-tests.txt

  Scenario: 3-way branch routes correctly
    Tool: Bash (grep verification)
    Preconditions: Code committed
    Steps:
      1. Read src/pipeline/sequence_processor.cpp around line 374
      2. Verify the branch structure: interpolateFactor>0 && scaleFactor>1 → RRFG, interpolateFactor>0 → FG, else → RR
      3. Verify processDirectoryRRFG is declared in sequence_processor.h
    Expected Result: 3-way branch present, new function declared
    Failure Indicators: Only 2-way branch, missing function declaration
    Evidence: .sisyphus/evidence/task-2-branch-logic.txt

  Scenario: DlssFG created at upscaled resolution (code review)
    Tool: Bash (grep verification)
    Preconditions: Code committed
    Steps:
      1. Search processDirectoryRRFG for createDlssFG call
      2. Verify it passes expectedOutputWidth, expectedOutputHeight (not render resolution)
    Expected Result: createDlssFG uses output (upscaled) dimensions
    Failure Indicators: Uses input (render) dimensions
    Evidence: .sisyphus/evidence/task-2-fg-resolution.txt

  Scenario: RR output passed directly to FG (code review)
    Tool: Bash (grep verification)
    Preconditions: Code committed
    Steps:
      1. Search processDirectoryRRFG for fgInput.backbuffer assignment
      2. Verify it uses rrOutput.image (not a re-uploaded texture)
      3. Verify no m_texturePipeline.download() between RR evaluate and FG evaluate
    Expected Result: RR output handle passed directly to FG input
    Failure Indicators: Download+re-upload pattern found between RR and FG
    Evidence: .sisyphus/evidence/task-2-gpu-passthrough.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): add combined RR→FG processing mode`
  - Files: `src/pipeline/sequence_processor.h`, `src/pipeline/sequence_processor.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 3. Integration Test for Combined RR+FG Mode

  **What to do**:
  - In `tests/test_integration.cpp`: Add a new test case `"dlss_rrfg_e2e_combined"` with tag `[integration][rrfg]`.
  - Follow the exact pattern of `dlss_fg_e2e_interpolation` (lines 76-151) but configure for combined mode.
  - Test structure:
    ```cpp
    TEST_CASE("dlss_rrfg_e2e_combined", "[integration][rrfg]") {
        struct OutputDirCleanup {
            ~OutputDirCleanup() { std::filesystem::remove_all("test_rrfg_integration_out/"); }
        } cleanup;

        std::filesystem::remove_all("test_rrfg_integration_out/");
        std::filesystem::create_directories("test_rrfg_integration_out/");

        VulkanContext ctx;
        std::string errorMsg;
        if (!ctx.init(errorMsg)) { SKIP("No RTX GPU"); }

        NgxContext ngx;
        if (!ngx.init(ctx.instance(), ctx.physicalDevice(), ctx.device(), nullptr, errorMsg)) {
            SKIP("NGX init failed: " + errorMsg);
        }
        if (!ngx.isDlssRRAvailable()) { SKIP("DLSS-RR not available"); }
        if (!ngx.isDlssFGAvailable()) { SKIP("DLSS-FG not available"); }

        {
            TexturePipeline pipeline(ctx);
            SequenceProcessor processor(ctx, ngx, pipeline);

            AppConfig config;
            config.inputDir = "tests/fixtures/sequence/";
            config.outputDir = "test_rrfg_integration_out/";
            config.scaleFactor = 2;
            config.interpolateFactor = 2;
            config.cameraDataFile = "tests/fixtures/camera.json";

            const bool result = processor.processDirectory(config.inputDir, config.outputDir, config, errorMsg);
            INFO(errorMsg);
            REQUIRE(result == true);

            REQUIRE(std::filesystem::exists(config.outputDir));
            // 5 input frames, 2x scale + 2x interpolation:
            // Frame 0: 1 upscaled output (frame_0001)
            // Frame pairs 1-4: each produces 1 interpolated + 1 upscaled = 2 outputs × 4 pairs = 8
            // Total: 1 + 8 = 9 output frames
            REQUIRE(countExrFiles(config.outputDir) == 9);

            // Verify output is at upscaled resolution (2x the input)
            // Test fixture is 64x64, so output should be 128x128
            const std::filesystem::path firstOutput = std::filesystem::path(config.outputDir) / "frame_0001.exr";
            REQUIRE(std::filesystem::exists(firstOutput));

            ExrReader reader;
            REQUIRE(reader.open(firstOutput.string(), errorMsg));
            REQUIRE(reader.width() == 128);
            REQUIRE(reader.height() == 128);

            // Verify an interpolated frame also exists at upscaled resolution
            const std::filesystem::path interpOutput = std::filesystem::path(config.outputDir) / "frame_0002.exr";
            REQUIRE(std::filesystem::exists(interpOutput));

            ExrReader interpReader;
            REQUIRE(interpReader.open(interpOutput.string(), errorMsg));
            REQUIRE(interpReader.width() == 128);
            REQUIRE(interpReader.height() == 128);

            // Verify interpolated frame has non-zero content
            const float* interpPixels = interpReader.readChannel("R");
            REQUIRE(interpPixels != nullptr);
            float maxPixel = 0.0f;
            const int pixelCount = interpReader.width() * interpReader.height();
            for (int i = 0; i < pixelCount; ++i) {
                maxPixel = std::max(maxPixel, interpPixels[i]);
            }
            REQUIRE(maxPixel > 1.0f);
        }

        ngx.shutdown();
        ctx.destroy();
    }
    ```
  - NOTE: The test fixture frames are 64×64 pixels. With `scaleFactor=2`, RR outputs at 128×128. FG interpolates at 128×128. So all output frames should be 128×128.
  - The frame count assertion (9) matches the FG-only test pattern: 5 inputs with 2x interpolation = 9 outputs (5 originals + 4 interpolated).

  **Must NOT do**:
  - Do NOT modify existing test cases
  - Do NOT change test fixtures
  - Do NOT make the test dependent on specific GPU model (use SKIP for capability checks)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Integration test requires understanding GPU pipeline behavior and output validation patterns
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3 (with T7, T10, T14)
  - **Blocks**: T4 (docs depend on verified functionality)
  - **Blocked By**: T2 (needs processDirectoryRRFG to exist)

  **References**:

  **Pattern References**:
  - `tests/test_integration.cpp:76-151` — `dlss_fg_e2e_interpolation` test (EXACT pattern to follow for RRFG test)
  - `tests/test_integration.cpp:28-74` — `e2e_sequence_processing` test (RR-only test for comparison)
  - `tests/test_integration.cpp:14-24` — `countExrFiles()` helper function (already exists, reuse it)

  **API/Type References**:
  - `src/pipeline/sequence_processor.h:17-24` — `SequenceProcessor` class and `processDirectory()` method
  - `src/cli/config.h:35-63` — `AppConfig` fields to set

  **WHY Each Reference Matters**:
  - Lines 76-151 — Exact test structure to copy: cleanup guard, GPU/NGX init with SKIP, config setup, frame count assertion, resolution assertion, pixel content assertion
  - Lines 14-24 — countExrFiles helper already exists in the anonymous namespace, just reuse it

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Test compiles and is discoverable
    Tool: Bash
    Preconditions: T2 committed, clean build
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe --list-tests
      3. Verify "dlss_rrfg_e2e_combined" appears in test list
    Expected Result: New test case visible in test listing
    Failure Indicators: Test not found, compile error
    Evidence: .sisyphus/evidence/task-3-test-listed.txt

  Scenario: Test runs (SKIP if no GPU)
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "dlss_rrfg_e2e_combined"
      2. Check output for PASS or SKIP
    Expected Result: Test passes on RTX 40+ GPU, or SKIPs with appropriate message on other hardware
    Failure Indicators: Test FAIL (not SKIP) — indicates a logic bug
    Evidence: .sisyphus/evidence/task-3-rrfg-test-run.txt

  Scenario: All existing tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All tests pass including new one
    Failure Indicators: Any regression
    Evidence: .sisyphus/evidence/task-3-all-tests.txt
  ```

  **Commit**: YES
  - Message: `test(pipeline): add integration test for combined RR+FG mode`
  - Files: `tests/test_integration.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 4. Update README.md and docs/usage_guide.md for Combined Mode

  **What to do**:
  - In `README.md`: Add a combined mode example in the CLI Usage section (after the "Frame interpolation (4x)" example):
    ```
    # Combined upscale + interpolation (2x each)
    dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --interpolate 2x --camera-data camera.json
    ```
  - In `docs/usage_guide.md`: Add a section explaining the combined mode:
    - What it does (upscales via DLSS-RR then interpolates via DLSS-FG)
    - Requirements (RTX 40+ for FG, DLSS-RR capable GPU for upscaling)
    - How output frames are numbered (interleaved original + interpolated)
    - Performance note: "Combined mode keeps RR output on GPU, avoiding intermediate disk I/O"
  - Also add `--memory-budget` to the docs.

  **Must NOT do**:
  - Do NOT add screenshots or images
  - Do NOT modify the project description or features list beyond what's needed
  - Do NOT add implementation details or API documentation

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Documentation-only task, small scope
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with T11)
  - **Blocks**: None
  - **Blocked By**: T2, T3 (docs should reflect verified functionality)

  **References**:

  **Pattern References**:
  - `README.md` lines with "CLI Usage" section — Follow existing example format
  - `docs/usage_guide.md` — Follow existing section structure

  **WHY Each Reference Matters**:
  - README CLI examples follow a specific comment+command format — new examples must match

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: README contains combined mode example
    Tool: Bash (grep)
    Preconditions: Files updated
    Steps:
      1. Search README.md for "--scale 2 --interpolate 2x"
      2. Verify the combined mode example exists
    Expected Result: Combined mode CLI example present in README
    Failure Indicators: Example missing
    Evidence: .sisyphus/evidence/task-4-readme-combined.txt

  Scenario: Usage guide mentions --memory-budget
    Tool: Bash (grep)
    Preconditions: Files updated
    Steps:
      1. Search docs/usage_guide.md for "--memory-budget"
      2. Verify the flag is documented
    Expected Result: --memory-budget documented in usage guide
    Failure Indicators: Flag not mentioned
    Evidence: .sisyphus/evidence/task-4-docs-memory-budget.txt
  ```

  **Commit**: YES
  - Message: `docs: update README and usage guide for combined mode and --memory-budget`
  - Files: `README.md`, `docs/usage_guide.md`
  - Pre-commit: None

- [ ] 5. TexturePipeline::uploadBatch() — Batch Texture Uploads

  **What to do**:
  - In `src/gpu/texture_pipeline.h`: Add a struct and method:
    ```cpp
    struct TextureUploadRequest {
        const float* data;
        int width;
        int height;
        int channels;
        VkFormat format;
    };

    std::vector<TextureHandle> uploadBatch(const std::vector<TextureUploadRequest>& requests);
    ```
  - In `src/gpu/texture_pipeline.cpp`: Implement `uploadBatch()`. The key optimization is using a SINGLE command buffer for ALL textures:

    1. Validate all requests (same `validateTextureArgs` as single upload)
    2. For each request:
       a. Create staging buffer (VMA_MEMORY_USAGE_AUTO + HOST_ACCESS_SEQUENTIAL_WRITE)
       b. Map memory, copy data (with float-to-half conversion if needed), flush, unmap
       c. Create VkImage (VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE) with same usage flags as single upload (TRANSFER_DST | TRANSFER_SRC | SAMPLED | STORAGE)
       d. Create VkImageView
    3. Begin ONE command buffer: `beginOneTimeCommands()`
    4. For each request: record transition (UNDEFINED → TRANSFER_DST), then vkCmdCopyBufferToImage, then transition (TRANSFER_DST → SHADER_READ_ONLY)
    5. End command buffer: `endOneTimeCommands()` — this does the single vkQueueSubmit + vkQueueWaitIdle
    6. Destroy ALL staging buffers
    7. Return vector of TextureHandles

    **Error handling**: If any image creation fails, destroy all previously created images and staging buffers before re-throwing. Follow the same try/catch pattern as single upload (lines 214-228).

    **Important**: The existing `beginOneTimeCommands()` / `endOneTimeCommands()` private methods handle command buffer allocation, begin, submit, and wait. They are already used by single `upload()`. The batch version records ALL transitions and copies into the same command buffer between begin and end.

  **Must NOT do**:
  - Do NOT modify the existing `upload()` method (it must continue working for single uploads)
  - Do NOT change `beginOneTimeCommands()` or `endOneTimeCommands()` signatures
  - Do NOT use multiple command buffers or multiple submits
  - Do NOT use C++20 features

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Vulkan resource management with careful error handling, multiple VkImage lifecycles in a single command buffer
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T1, T8, T12)
  - **Blocks**: T6 (integration)
  - **Blocked By**: None — can start immediately

  **References**:

  **Pattern References**:
  - `src/gpu/texture_pipeline.cpp:94-229` — Existing `upload()` method (EXACT pattern to replicate per-texture, but batch the command buffer)
  - `src/gpu/texture_pipeline.cpp:109-119` — Staging buffer creation with VMA
  - `src/gpu/texture_pipeline.cpp:124-138` — Memory mapping, float-to-half conversion, flush
  - `src/gpu/texture_pipeline.cpp:146-183` — VkImage + VkImageView creation
  - `src/gpu/texture_pipeline.cpp:185-210` — Command buffer: transition → copy → transition → submit (this is what gets batched)
  - `src/gpu/texture_pipeline.cpp:214-228` — Error handling / cleanup pattern

  **API/Type References**:
  - `src/gpu/texture_pipeline.h:15-23` — `TextureHandle` struct (return type)
  - `src/gpu/texture_pipeline.h:39-40` — `beginOneTimeCommands()` / `endOneTimeCommands()` (reuse these)

  **WHY Each Reference Matters**:
  - Lines 94-229 — The entire single upload is the template. Batch version creates all resources first, then records all GPU commands into one command buffer.
  - Lines 185-210 — This is the "hot path" that currently runs 8 times per frame. Batching this into a single begin/end eliminates 7 of 8 GPU sync points.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with uploadBatch
    Tool: Bash
    Preconditions: None
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build
    Failure Indicators: Compile/linker errors
    Evidence: .sisyphus/evidence/task-5-build.txt

  Scenario: Existing upload roundtrip test still passes
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "roundtrip_upload_download"
      2. Verify PASS
    Expected Result: Existing test unaffected
    Failure Indicators: Test fails (regression in shared code)
    Evidence: .sisyphus/evidence/task-5-existing-roundtrip.txt

  Scenario: All existing tests pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: No regressions
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-5-all-tests.txt
  ```

  **Commit**: YES
  - Message: `perf(gpu): add batch texture upload method`
  - Files: `src/gpu/texture_pipeline.h`, `src/gpu/texture_pipeline.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 6. Integrate Batch Uploads into Processing Functions

  **What to do**:
  - In `src/pipeline/sequence_processor.cpp`: Replace the 8 individual `m_texturePipeline.upload()` calls in the RR-only path (lines 500-539) with a single `m_texturePipeline.uploadBatch()` call.
  - Replace the 3 individual uploads in `processDirectoryFG()` (lines 841-855) with `uploadBatch()`.
  - Replace the uploads in the new `processDirectoryRRFG()` with `uploadBatch()`.
  - For each processing function, construct a `std::vector<TextureUploadRequest>` and call `uploadBatch()`, then unpack the returned vector into individual TextureHandle variables.

  **RR-only path** (lines 500-539):
  ```cpp
  std::vector<TextureUploadRequest> requests = {
      {mappedBuffers.color.data(), expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
      {mappedBuffers.depth.data(), expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT},
      {mvResult.mvXY.data(), expectedInputWidth, expectedInputHeight, 2, VK_FORMAT_R16G16_SFLOAT},
      {diffuseRgba.data(), expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
      {specularRgba.data(), expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
      {normalsRgba.data(), expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
      {mappedBuffers.roughness.data(), expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT},
      {outputInit.data(), expectedOutputWidth, expectedOutputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
  };
  auto handles = m_texturePipeline.uploadBatch(requests);
  TextureHandle color = handles[0], depth = handles[1], motion = handles[2];
  TextureHandle diffuse = handles[3], specular = handles[4], normals = handles[5];
  TextureHandle roughness = handles[6], output = handles[7];
  ```

  **FG path** (lines 841-855):
  ```cpp
  std::vector<TextureUploadRequest> requests = {
      {mappedBuffers.color.data(), expectedWidth, expectedHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT},
      {mappedBuffers.depth.data(), expectedWidth, expectedHeight, 1, VK_FORMAT_R32_SFLOAT},
      {mvResult.mvXY.data(), expectedWidth, expectedHeight, 2, VK_FORMAT_R16G16_SFLOAT},
  };
  auto handles = m_texturePipeline.uploadBatch(requests);
  TextureHandle color = handles[0], depth = handles[1], motion = handles[2];
  ```

  **RRFG path**: Same pattern as RR for the 8-texture upload, then individual uploads for outputInterp in the FG inner loop (can't batch those since they're per-multiFrameIndex).

  - Include `texture_pipeline.h` is already included in sequence_processor.cpp (line 12). The `TextureUploadRequest` struct will be available.
  - The destroy pattern remains the same (individual destroyHandle calls per handle).

  **Must NOT do**:
  - Do NOT remove the individual `upload()` method from TexturePipeline
  - Do NOT change the destroy/cleanup pattern
  - Do NOT batch the FG inner loop's outputInterp uploads (they are 1 per iteration, created and destroyed within the loop)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Mechanical but important refactoring across 3 functions, must maintain correct texture ordering
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with T2, T9, T13)
  - **Blocks**: T7 (batch upload tests)
  - **Blocked By**: T5 (needs uploadBatch method)

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:500-539` — 8 individual RR uploads to replace
  - `src/pipeline/sequence_processor.cpp:841-855` — 3 individual FG uploads to replace
  - `src/pipeline/sequence_processor.cpp:625-632` — destroyHandle cleanup (keep unchanged)

  **API/Type References**:
  - `src/gpu/texture_pipeline.h` — `TextureUploadRequest` struct and `uploadBatch()` (from T5)

  **WHY Each Reference Matters**:
  - Lines 500-539 — These 8 sequential uploads are the primary performance bottleneck. Replacing them with uploadBatch() is the #1 optimization.
  - Lines 841-855 — FG path also benefits (3 uploads → 1 batch).

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with batch integration
    Tool: Bash
    Preconditions: T5 committed
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build
    Failure Indicators: Compile errors
    Evidence: .sisyphus/evidence/task-6-build.txt

  Scenario: All existing tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: No regressions — batch upload produces identical results to individual uploads
    Failure Indicators: Integration tests fail (output mismatch)
    Evidence: .sisyphus/evidence/task-6-all-tests.txt

  Scenario: No individual upload calls remain in main paths (code review)
    Tool: Bash (grep)
    Preconditions: Code committed
    Steps:
      1. In processDirectory RR path: verify uploadBatch is used instead of 8 individual upload calls
      2. In processDirectoryFG: verify uploadBatch is used
      3. In processDirectoryRRFG: verify uploadBatch is used
    Expected Result: All 3 paths use uploadBatch for their main texture set
    Failure Indicators: Individual upload() calls still present for main texture sets
    Evidence: .sisyphus/evidence/task-6-batch-usage.txt
  ```

  **Commit**: YES
  - Message: `perf(pipeline): integrate batch uploads into processing functions`
  - Files: `src/pipeline/sequence_processor.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 7. Tests for uploadBatch

  **What to do**:
  - In `tests/test_texture_pipeline.cpp`: Add test cases for `uploadBatch()`:

    **Test 1: "batch_upload_download_roundtrip"** `[gpu][texture_pipeline]`:
    - Create 3 different test data vectors (different formats/sizes):
      - 64×64 RGBA (R16G16B16A16_SFLOAT)
      - 64×64 depth (R32_SFLOAT)
      - 64×64 RG motion (R16G16_SFLOAT)
    - Call `uploadBatch()` with all 3
    - Verify returned vector has size 3
    - Download each handle and verify data matches input (within half-float precision tolerance ~1e-3)
    - Destroy all handles

    **Test 2: "batch_upload_empty_request"** `[gpu][texture_pipeline]`:
    - Call `uploadBatch()` with empty vector
    - Verify returned vector is empty (no crash, no error)

    **Test 3: "batch_upload_single_request"** `[gpu][texture_pipeline]`:
    - Call `uploadBatch()` with 1 request
    - Verify it works identically to individual `upload()`
    - Download and verify data roundtrip

  **Must NOT do**:
  - Do NOT modify existing test cases
  - Do NOT depend on specific GPU model (use SKIP pattern)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: GPU tests require Vulkan context setup with proper SKIP handling
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3 (with T3, T10, T14)
  - **Blocks**: None
  - **Blocked By**: T6 (needs batch upload integrated to verify correctness end-to-end)

  **References**:

  **Pattern References**:
  - `tests/test_texture_pipeline.cpp:10-40` — `roundtrip_upload_download` test (EXACT pattern: VulkanContext init with SKIP, create pipeline, upload, download, compare, destroy)

  **API/Type References**:
  - `src/gpu/texture_pipeline.h` — `TextureUploadRequest` struct, `uploadBatch()` method

  **WHY Each Reference Matters**:
  - Lines 10-40 — The only existing texture pipeline test. Follow its VulkanContext init pattern, SKIP on no GPU, and tolerance-based comparison.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Batch roundtrip test passes
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe "batch_upload_download_roundtrip"
      3. Verify PASS or SKIP
    Expected Result: Test passes on GPU-equipped machine, SKIPs otherwise
    Failure Indicators: Test FAIL (data mismatch after batch upload/download)
    Evidence: .sisyphus/evidence/task-7-batch-roundtrip.txt

  Scenario: All tests pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All tests pass
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-7-all-tests.txt
  ```

  **Commit**: YES
  - Message: `test(gpu): add tests for uploadBatch`
  - Files: `tests/test_texture_pipeline.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 8. TexturePool Class — Cross-Frame VkImage Reuse

  **What to do**:
  - Create NEW file `src/gpu/texture_pool.h`:
    ```cpp
    #pragma once

    #ifndef VK_USE_PLATFORM_WIN32_KHR
    #define VK_USE_PLATFORM_WIN32_KHR
    #endif

    #include <volk.h>
    #include "gpu/texture_pipeline.h"

    #include <cstdint>
    #include <unordered_map>
    #include <vector>
    #include <tuple>
    #include <functional>

    class VulkanContext;

    class TexturePool {
    public:
        explicit TexturePool(VulkanContext& ctx, TexturePipeline& pipeline, int64_t maxMemoryBytes);
        ~TexturePool();

        // Acquire a texture from the pool (creates if not cached, reuses if available)
        // Returns a TextureHandle with valid VkImage/VkImageView at the requested dimensions
        TextureHandle acquire(int width, int height, int channels, VkFormat format);

        // Update pixel data into an existing pooled texture (reuses staging buffer)
        void updateData(TextureHandle& handle, const float* data);

        // Release a specific handle back to the pool (does NOT destroy, marks as available)
        void release(TextureHandle& handle);

        // Destroy all pooled resources
        void releaseAll();

        // Query current memory usage
        int64_t currentMemoryBytes() const;

    private:
        struct PoolKey {
            int width;
            int height;
            int channels;
            VkFormat format;
            bool operator==(const PoolKey& other) const;
        };

        struct PoolKeyHash {
            size_t operator()(const PoolKey& key) const;
        };

        VulkanContext& m_ctx;
        TexturePipeline& m_pipeline;
        int64_t m_maxMemoryBytes;
        int64_t m_currentMemoryBytes = 0;
        std::unordered_map<PoolKey, std::vector<TextureHandle>, PoolKeyHash> m_available;
        std::vector<TextureHandle> m_inUse;
    };
    ```
  - Create NEW file `src/gpu/texture_pool.cpp`:
    - `acquire()`: Check `m_available[key]` for a cached handle. If found, move to `m_inUse` and return. If not, create via `m_pipeline.upload()` with zeroed data (or direct VkImage creation matching the pattern in texture_pipeline.cpp:146-183), add to `m_inUse`, track memory.
    - `updateData()`: Create staging buffer, map, copy data (with float-to-half if needed), flush, unmap. Record transition (SHADER_READ_ONLY → TRANSFER_DST), copy, transition back (TRANSFER_DST → SHADER_READ_ONLY) in a single command buffer. Destroy staging buffer. This avoids recreating the VkImage — only the pixel data changes.
    - `release()`: Move handle from `m_inUse` to `m_available[key]`. Do NOT destroy the VkImage.
    - `releaseAll()`: Destroy all handles in both `m_available` and `m_inUse`. Reset memory counter.
    - `currentMemoryBytes()`: Return `m_currentMemoryBytes`.
    - Memory tracking: Each allocation adds `width * height * bytesPerPixel(format)` to the counter. If `m_currentMemoryBytes + newSize > m_maxMemoryBytes`, evict least-recently-used entries from `m_available` before allocating.

  - In `CMakeLists.txt` (line 111-122): Add `src/gpu/texture_pool.cpp` to `dlss_compositor_core` sources.
  - In `tests/CMakeLists.txt`: No changes needed (test file will be added in T10).

  **Must NOT do**:
  - Do NOT use C++20 features (no std::span, no concepts)
  - Do NOT use global state or singletons
  - Do NOT modify TexturePipeline class
  - Do NOT use std::mutex (TexturePool is single-threaded, called from the processing loop)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: New class with Vulkan resource lifecycle management, memory tracking, pool eviction logic
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T1, T5, T12)
  - **Blocks**: T9 (integration), T10 (tests)
  - **Blocked By**: None — can start immediately

  **References**:

  **Pattern References**:
  - `src/gpu/texture_pipeline.cpp:94-229` — `upload()` method: staging buffer creation, VkImage creation, layout transitions, command buffer submit. TexturePool's `updateData()` will reuse the same Vulkan patterns but skip VkImage creation.
  - `src/gpu/texture_pipeline.cpp:146-169` — VkImage + VmaAllocation creation pattern (for pool's `acquire()` when creating new handles)
  - `src/gpu/texture_pipeline.cpp:171-183` — VkImageView creation pattern
  - `src/gpu/texture_pipeline.h:15-23` — `TextureHandle` struct (pool stores and returns these)
  - `src/gpu/texture_pipeline.h:27-47` — `TexturePipeline` class (pool uses its `upload()`/`destroy()` for handle lifecycle)

  **API/Type References**:
  - VMA: `vmaCreateBuffer`, `vmaMapMemory`, `vmaFlushAllocation`, `vmaUnmapMemory`, `vmaDestroyBuffer` — for staging buffers
  - VMA: `vmaCreateImage`, `vmaDestroyImage` — for VkImage lifecycle
  - Vulkan: `vkCreateImageView`, `vkDestroyImageView`, `vkCmdPipelineBarrier`, `vkCmdCopyBufferToImage`

  **WHY Each Reference Matters**:
  - Lines 94-229 — The pool's `updateData()` is essentially a stripped-down upload: create staging, copy data, transition, buffer→image copy, transition, destroy staging. No VkImage creation.
  - Lines 146-183 — Pool's `acquire()` needs to create VkImage+VkImageView when cache miss occurs. Follow this exact pattern.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with TexturePool
    Tool: Bash
    Preconditions: None
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build with TexturePool compiled and linked
    Failure Indicators: Compile/linker errors
    Evidence: .sisyphus/evidence/task-8-build.txt

  Scenario: TexturePool added to CMakeLists
    Tool: Bash (grep)
    Preconditions: Files created
    Steps:
      1. Search CMakeLists.txt for "texture_pool.cpp"
      2. Verify it's listed in dlss_compositor_core sources
    Expected Result: texture_pool.cpp in source list
    Failure Indicators: Missing from CMakeLists
    Evidence: .sisyphus/evidence/task-8-cmake.txt

  Scenario: All existing tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: No regressions from adding new files
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-8-all-tests.txt
  ```

  **Commit**: YES
  - Message: `perf(gpu): add TexturePool for cross-frame VkImage reuse`
  - Files: `src/gpu/texture_pool.h`, `src/gpu/texture_pool.cpp`, `CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 9. Integrate TexturePool into Processing Functions

  **What to do**:
  - In `src/pipeline/sequence_processor.h`: Add `#include "gpu/texture_pool.h"` or forward declare `TexturePool`.
  - Modify `SequenceProcessor` constructor and member: The TexturePool should be created per-processing-run (not a class member) since it needs the memory budget from config. Create it at the top of each processing function.
  - In each processing function (`processDirectory` RR path, `processDirectoryFG`, `processDirectoryRRFG`):

    **Strategy**: Create TexturePool at the start of the processing loop (after determining frame dimensions). Use `pool.acquire()` / `pool.updateData()` / `pool.release()` instead of `upload()` / `destroy()`.

    **RR-only path** (lines 490-632):
    ```cpp
    // Before the frame loop, after dimensions are known:
    const int64_t maxMemory = static_cast<int64_t>(config.memoryBudgetGB) * 1024LL * 1024LL * 1024LL;
    TexturePool pool(m_ctx, m_texturePipeline, maxMemory);

    // First frame: acquire handles
    TextureHandle color = pool.acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    // ... acquire all 8 handles

    // Each frame: updateData instead of upload+destroy
    pool.updateData(color, mappedBuffers.color.data());
    // ... updateData for all textures

    // After processing all frames:
    pool.releaseAll();
    ```

    **FG path** (lines 836-945):
    Same pattern but 3 textures (color, depth, motion). The `outputInterp` texture is still created and destroyed per-multiFrameIndex inside the inner loop (it could be pooled but the gain is minimal and adds complexity).

    **RRFG path**:
    Same pattern as RR for the 8 input textures + RR output. FG's outputInterp still per-loop.

    **Key change**: Replace the per-frame `upload()`/`destroy()` cycle with `acquire()` once + `updateData()` per frame + `releaseAll()` at end. This eliminates VkImage/VkImageView/VmaAllocation creation and destruction per frame.

  **Must NOT do**:
  - Do NOT pool the outputInterp texture in the FG inner loop (it changes per multiFrameIndex)
  - Do NOT make TexturePool a class member of SequenceProcessor (it's per-run with config-dependent budget)
  - Do NOT modify TexturePipeline class

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Mechanical integration across 3 functions, must track acquire/updateData/release lifecycle correctly
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with T2, T6, T13)
  - **Blocks**: T10 (pool tests), T11 (--memory-budget test)
  - **Blocked By**: T8 (needs TexturePool class)

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:490-539` — Current per-frame upload calls to replace with pool.acquire() + pool.updateData()
  - `src/pipeline/sequence_processor.cpp:625-632` — Current per-frame destroy calls to replace with pool.release()
  - `src/pipeline/sequence_processor.cpp:836-945` — FG per-frame upload/destroy to replace

  **API/Type References**:
  - `src/gpu/texture_pool.h` — TexturePool::acquire(), updateData(), release(), releaseAll() (from T8)
  - `src/cli/config.h` — `config.memoryBudgetGB` field (from T1)

  **WHY Each Reference Matters**:
  - Lines 490-539 / 625-632 — These are the exact code blocks being refactored. The new pattern replaces upload()+destroy() per frame with acquire() once + updateData() per frame.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with pool integration
    Tool: Bash
    Preconditions: T8 committed
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build
    Failure Indicators: Compile errors
    Evidence: .sisyphus/evidence/task-9-build.txt

  Scenario: All existing integration tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: Pool produces identical output to non-pooled path
    Failure Indicators: Integration test failures (output mismatch)
    Evidence: .sisyphus/evidence/task-9-all-tests.txt
  ```

  **Commit**: YES
  - Message: `perf(pipeline): integrate TexturePool into processing functions`
  - Files: `src/pipeline/sequence_processor.h`, `src/pipeline/sequence_processor.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 10. Tests for TexturePool

  **What to do**:
  - Create `tests/test_texture_pool.cpp` with Catch2 tests:

    **Test 1: "texture_pool_acquire_and_release"** `[gpu][texture_pool]`:
    - Create TexturePool with 1GB budget
    - Acquire a 64×64 RGBA handle
    - Verify handle has valid VkImage/VkImageView
    - Release it back to pool
    - Acquire same dimensions again — should get the cached handle (verify VkImage is the same pointer)
    - releaseAll()

    **Test 2: "texture_pool_update_data_roundtrip"** `[gpu][texture_pool]`:
    - Acquire a 64×64 RGBA handle
    - updateData with test pattern
    - Download via TexturePipeline::download()
    - Verify data matches (within half-float tolerance)
    - updateData with different pattern
    - Download again, verify second pattern
    - releaseAll()

    **Test 3: "texture_pool_release_all"** `[gpu][texture_pool]`:
    - Acquire 3 handles of different sizes
    - releaseAll()
    - Verify currentMemoryBytes() == 0

    **Test 4: "texture_pool_memory_budget"** `[gpu][texture_pool]`:
    - Create TexturePool with very small budget (e.g., 1MB)
    - Acquire a texture that fits
    - Release it
    - Acquire a different large texture that would exceed budget
    - Verify the old cached entry was evicted (currentMemoryBytes stays under budget)
    - releaseAll()

  - In `tests/CMakeLists.txt`: Add `test_texture_pool.cpp` to the test executable sources.

  **Must NOT do**:
  - Do NOT modify existing test files
  - Do NOT depend on specific GPU capabilities beyond basic Vulkan

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: GPU tests with Vulkan context, pool lifecycle verification
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3 (with T3, T7, T14)
  - **Blocks**: None
  - **Blocked By**: T9 (needs pool integrated to verify full lifecycle)

  **References**:

  **Pattern References**:
  - `tests/test_texture_pipeline.cpp:10-40` — VulkanContext init, SKIP pattern, upload/download roundtrip verification
  - `tests/CMakeLists.txt:2-14` — How test sources are listed

  **API/Type References**:
  - `src/gpu/texture_pool.h` — TexturePool API (from T8)
  - `src/gpu/texture_pipeline.h` — TexturePipeline::download() for roundtrip verification

  **WHY Each Reference Matters**:
  - test_texture_pipeline.cpp — Exact test infrastructure pattern to follow (VulkanContext init, SKIP, pipeline creation)

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Pool tests compile and run
    Tool: Bash
    Preconditions: T9 committed, build succeeds
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe "[texture_pool]"
      3. Verify all pass or SKIP
    Expected Result: Pool tests pass on GPU-equipped machine
    Failure Indicators: Test failures
    Evidence: .sisyphus/evidence/task-10-pool-tests.txt

  Scenario: All tests pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All tests pass
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-10-all-tests.txt
  ```

  **Commit**: YES
  - Message: `test(gpu): add tests for TexturePool`
  - Files: `tests/test_texture_pool.cpp`, `tests/CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 11. CLI Test for --memory-budget Flag

  **What to do**:
  - In `tests/test_cli.cpp`: Verify the --memory-budget test cases from T1 exist and add any missing:
    - "CliParser - --memory-budget default" — Verify default is 8 when flag not specified
    - Ensure the tests from T1 (valid, invalid, missing value) cover all edge cases

    Note: The actual test cases were already added in T1. This task is a verification pass and adds the "default value" test if it's missing.

  **Must NOT do**:
  - Do NOT duplicate tests already added in T1

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple test addition/verification
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with T4)
  - **Blocks**: None
  - **Blocked By**: T1 (needs --memory-budget parsing), T9 (needs pool integration for full verification)

  **References**:

  **Pattern References**:
  - `tests/test_cli.cpp:8-15` — FakeArgs pattern
  - `tests/test_cli.cpp:111-146` — Existing --interpolate tests (pattern to follow)

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Default memory budget test passes
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe "CliParser - --memory-budget default"
      3. Verify PASS
    Expected Result: Default memoryBudgetGB is 8
    Failure Indicators: Test fails or doesn't exist
    Evidence: .sisyphus/evidence/task-11-default-budget.txt

  Scenario: All tests pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All tests pass
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-11-all-tests.txt
  ```

  **Commit**: YES
  - Message: `test(cli): add default value test for --memory-budget flag`
  - Files: `tests/test_cli.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 12. FramePrefetcher Class — Async EXR Prefetch with Background Thread

  **What to do**:
  - Create NEW file `src/pipeline/frame_prefetcher.h`:
    ```cpp
    #pragma once
    #include "core/channel_mapper.h"
    #include "core/exr_reader.h"
    #include "core/mv_converter.h"
    #include "pipeline/sequence_processor.h"  // for SequenceFrameInfo

    #include <thread>
    #include <mutex>
    #include <condition_variable>
    #include <vector>
    #include <string>
    #include <atomic>
    #include <optional>

    struct PrefetchedFrame {
        SequenceFrameInfo frameInfo;
        MappedBuffers mappedBuffers;
        MvConverter::MvResult mvResult;
        bool valid = false;
    };

    class FramePrefetcher {
    public:
        // ringSize = number of frames to prefetch ahead (e.g., 2-4)
        FramePrefetcher(const ChannelMapper& mapper, const MvConverter& converter,
                        int ringSize, int expectedWidth, int expectedHeight);
        ~FramePrefetcher();

        // Submit a batch of frames to prefetch (call once before processing loop or when frame list changes)
        void start(const std::vector<SequenceFrameInfo>& frames);

        // Get the next prefetched frame (blocks if not ready yet, returns nullopt if all consumed)
        std::optional<PrefetchedFrame> getNext();

        // Stop the background thread (called by destructor, safe to call multiple times)
        void stop();

    private:
        void workerLoop();

        const ChannelMapper& m_mapper;
        const MvConverter& m_converter;
        int m_ringSize;
        int m_expectedWidth;
        int m_expectedHeight;

        std::vector<SequenceFrameInfo> m_frames;
        std::vector<PrefetchedFrame> m_ring;  // ring buffer of prefetched frames
        int m_writeIndex = 0;     // next slot to write into (background thread)
        int m_readIndex = 0;      // next slot to read from (main thread)
        int m_nextFrameToLoad = 0; // index into m_frames for next frame to load

        std::thread m_worker;
        std::mutex m_mutex;
        std::condition_variable m_cvProduced;  // signaled when a frame is ready
        std::condition_variable m_cvConsumed;  // signaled when a slot is freed
        std::atomic<bool> m_stopRequested{false};
        bool m_started = false;
    };
    ```

  - Create NEW file `src/pipeline/frame_prefetcher.cpp`:

    **Constructor**: Store references, allocate ring buffer of `ringSize` PrefetchedFrame entries.

    **start()**: Store frames list, reset indices, launch `m_worker = std::thread(&FramePrefetcher::workerLoop, this)`.

    **workerLoop()**: Background thread function:
    ```cpp
    void FramePrefetcher::workerLoop() {
        while (!m_stopRequested.load()) {
            std::unique_lock<std::mutex> lock(m_mutex);

            // Wait until there's a free slot in the ring AND frames remain to load
            m_cvConsumed.wait(lock, [this] {
                return m_stopRequested.load() ||
                       (m_nextFrameToLoad < static_cast<int>(m_frames.size()) &&
                        ((m_writeIndex + 1) % m_ringSize) != m_readIndex);
            });

            if (m_stopRequested.load()) break;
            if (m_nextFrameToLoad >= static_cast<int>(m_frames.size())) break;

            // Grab the next frame info
            int frameIdx = m_nextFrameToLoad++;
            const auto& info = m_frames[frameIdx];
            lock.unlock();

            // CPU-intensive work: read EXR + map channels + convert MVs (no lock held)
            PrefetchedFrame pf;
            pf.frameInfo = info;
            try {
                ExrReader reader(info.path);
                pf.mappedBuffers = m_mapper.mapFromExr(reader, m_expectedWidth, m_expectedHeight);
                pf.mvResult = m_converter.convert(
                    pf.mappedBuffers.motionVectors, m_expectedWidth, m_expectedHeight);
                pf.valid = true;
            } catch (...) {
                pf.valid = false;
            }

            // Place result in ring
            lock.lock();
            m_ring[m_writeIndex] = std::move(pf);
            m_writeIndex = (m_writeIndex + 1) % m_ringSize;
            m_cvProduced.notify_one();
        }
    }
    ```

    **getNext()**: Main thread calls this:
    ```cpp
    std::optional<PrefetchedFrame> FramePrefetcher::getNext() {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_cvProduced.wait(lock, [this] {
            return m_stopRequested.load() || m_readIndex != m_writeIndex;
        });

        if (m_stopRequested.load() && m_readIndex == m_writeIndex) return std::nullopt;
        if (m_readIndex == m_writeIndex) return std::nullopt;

        PrefetchedFrame result = std::move(m_ring[m_readIndex]);
        m_readIndex = (m_readIndex + 1) % m_ringSize;
        m_cvConsumed.notify_one();
        return result;
    }
    ```

    **stop()**: Set `m_stopRequested = true`, notify both CVs, join thread if joinable.

    **Destructor**: Call `stop()`.

  - In `CMakeLists.txt` (line 111-122): Add `src/pipeline/frame_prefetcher.cpp` to `dlss_compositor_core` sources.

  **Must NOT do**:
  - Do NOT use C++20 features (`std::jthread`, `std::stop_token`, etc.)
  - Do NOT use `std::async` or thread pools — single dedicated `std::thread`
  - Do NOT hold mutex during EXR read (that's the whole point — I/O runs unlocked)
  - Do NOT modify ExrReader, ChannelMapper, or MvConverter
  - Do NOT use global state

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Multi-threaded ring buffer with mutex/condition_variable, careful lock scope management, exception safety in worker thread
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with T1, T5, T8)
  - **Blocks**: T13 (integration), T14 (tests)
  - **Blocked By**: None — can start immediately

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:414-500` — Current RR frame loop: EXR read at L429-434, channel mapping at L436-443, MV conversion at L445-460. This is exactly what the prefetcher's workerLoop replaces (doing the same work on a background thread).
  - `src/pipeline/sequence_processor.cpp:775-840` — Current FG frame loop: same pattern (EXR read, map, MV convert). Prefetcher handles this too.
  - `src/core/channel_mapper.h:41-57` — `MappedBuffers` struct: what the prefetcher produces per frame (color, depth, motionVectors, diffuse, specular, normals, roughness buffers).
  - `src/core/channel_mapper.h:63` — `ChannelMapper::mapFromExr()`: called by workerLoop to map EXR channels to MappedBuffers.
  - `src/core/mv_converter.h` — `MvConverter::convert()` returns `MvResult` with `mvXY` vector.
  - `src/core/exr_reader.h` — `ExrReader` constructor takes path, used to read each EXR file.
  - `src/pipeline/sequence_processor.h:12-15` — `SequenceFrameInfo` struct: `path` (string), `frameNumber` (int), used as input to the prefetcher.

  **Threading References** (existing patterns in codebase):
  - `src/ui/app.cpp` — Uses `std::thread` for background processing
  - `src/ui/settings_panel.h` — Uses `std::mutex`, `std::condition_variable` for thread sync

  **WHY Each Reference Matters**:
  - Lines 414-500 / 775-840 — These frame loops do sequential EXR read → map → MV convert → GPU upload. The prefetcher decouples the CPU work (read+map+convert) from GPU work (upload+process+download), running them in parallel.
  - `MappedBuffers` struct — The prefetcher stores this per-frame; the main thread consumes it directly for GPU upload, skipping the EXR read wait.
  - `SequenceFrameInfo` — Input type that tells the prefetcher which frames to load and in what order.
  - Threading refs — Confirm the project already uses std::thread/mutex/CV, so FramePrefetcher follows established patterns.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with FramePrefetcher
    Tool: Bash
    Preconditions: None
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build with FramePrefetcher compiled and linked
    Failure Indicators: Compile/linker errors (missing includes, undefined symbols)
    Evidence: .sisyphus/evidence/task-12-build.txt

  Scenario: FramePrefetcher added to CMakeLists
    Tool: Bash (grep)
    Preconditions: Files created
    Steps:
      1. Search CMakeLists.txt for "frame_prefetcher.cpp"
      2. Verify it's listed in dlss_compositor_core sources
    Expected Result: frame_prefetcher.cpp in source list
    Failure Indicators: Missing from CMakeLists
    Evidence: .sisyphus/evidence/task-12-cmake.txt

  Scenario: All existing tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: No regressions from adding new files
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-12-all-tests.txt

  Scenario: No C++20 features used
    Tool: Bash (grep)
    Preconditions: Code written
    Steps:
      1. Search src/pipeline/frame_prefetcher.h and .cpp for "jthread", "stop_token", "std::span", "concept ", "requires "
      2. Verify zero matches
    Expected Result: No C++20-only features found
    Failure Indicators: Any C++20 feature detected
    Evidence: .sisyphus/evidence/task-12-no-cpp20.txt
  ```

  **Commit**: YES
  - Message: `perf(pipeline): add async FramePrefetcher with background thread`
  - Files: `src/pipeline/frame_prefetcher.h`, `src/pipeline/frame_prefetcher.cpp`, `CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 13. Integrate FramePrefetcher into Processing Functions

  **What to do**:
  - In `src/pipeline/sequence_processor.cpp`: Replace the sequential EXR read → map → MV convert pattern in all 3 processing functions with FramePrefetcher.

  **For each processing function** (`processDirectory` RR path, `processDirectoryFG`, and `processDirectoryRRFG`):

  1. **Before the frame loop**: Create prefetcher and start it:
     ```cpp
     #include "pipeline/frame_prefetcher.h"

     // At the top of the processing function, after building the frame list:
     FramePrefetcher prefetcher(m_channelMapper, m_mvConverter, 3,
                                 expectedInputWidth, expectedInputHeight);
     prefetcher.start(sortedFrames);  // sortedFrames is the existing std::vector<SequenceFrameInfo>
     ```

  2. **Inside the frame loop**: Replace EXR read + map + MV convert with getNext():

     **RR-only path** — currently lines 414-460 do:
     ```cpp
     // CURRENT (sequential):
     ExrReader reader(info.path);
     auto mappedBuffers = m_channelMapper.mapFromExr(reader, expectedInputWidth, expectedInputHeight);
     auto mvResult = m_mvConverter.convert(mappedBuffers.motionVectors, ...);
     ```
     Replace with:
     ```cpp
     // NEW (prefetched):
     auto prefetched = prefetcher.getNext();
     if (!prefetched || !prefetched->valid) {
         spdlog::error("Failed to prefetch frame: {}", info.path);
         continue;
     }
     auto& mappedBuffers = prefetched->mappedBuffers;
     auto& mvResult = prefetched->mvResult;
     // Rest of the loop body remains identical — GPU upload + DLSS evaluate + download
     ```

     **FG path** — same pattern at lines 775-810:
     Replace ExrReader + mapFromExr + convert block with `prefetcher.getNext()`.

     **RRFG path** — same pattern in the new combined function from T2.

  3. **After the frame loop**: Prefetcher destructor handles stop+join automatically, but explicit `prefetcher.stop()` is fine for clarity.

  **Ring size choice**: Use 3 (prefetch 2 frames ahead). This is enough to hide EXR I/O latency without excessive memory usage. Each prefetched frame holds ~100MB of mapped buffers for a 1920×1080 frame with all channels.

  **Important edge cases**:
  - If `prefetched->valid == false`, log a warning and `continue` (skip that frame). This matches the current behavior where EXR read failures throw and are caught at the loop level.
  - The frame iteration variable (`info`) from the current `for` loop is no longer needed for EXR reading, but keep it for logging/progress reporting OR use `prefetched->frameInfo` instead.
  - The prefetcher takes `const ChannelMapper&` and `const MvConverter&` — these must outlive the prefetcher. They are class members of SequenceProcessor, so this is safe.

  **Must NOT do**:
  - Do NOT remove the ExrReader, ChannelMapper, or MvConverter includes (they're used elsewhere)
  - Do NOT change the GPU upload/evaluate/download portion of the loop
  - Do NOT modify the FramePrefetcher class (implemented in T12)
  - Do NOT change the frame sorting/filtering logic before the loop
  - Do NOT change the output file writing logic after DLSS evaluate

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Mechanical integration in 3 functions, replacing sequential read with async prefetch, must preserve loop structure and error handling
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with T2, T6, T9)
  - **Blocks**: T14 (prefetcher tests)
  - **Blocked By**: T12 (needs FramePrefetcher class)

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:414-460` — RR frame loop: EXR read (L429-434), channel mapping (L436-443), MV conversion (L445-460). Replace this block with `prefetcher.getNext()`.
  - `src/pipeline/sequence_processor.cpp:775-810` — FG frame loop: same EXR read + map + convert pattern. Replace with `prefetcher.getNext()`.
  - `src/pipeline/sequence_processor.cpp:390-410` — Frame list construction + sorting (sortedFrames vector). This becomes the input to `prefetcher.start()`.
  - `src/pipeline/sequence_processor.cpp:461-539` — GPU upload section (KEEP UNCHANGED — this runs after prefetch completes, using prefetched mappedBuffers).

  **API/Type References**:
  - `src/pipeline/frame_prefetcher.h` — `FramePrefetcher::start()`, `getNext()`, `PrefetchedFrame` struct (from T12)
  - `src/pipeline/sequence_processor.h:12-15` — `SequenceFrameInfo` struct (input to prefetcher)
  - `src/core/channel_mapper.h:41-57` — `MappedBuffers` struct (output of prefetcher, consumed by GPU upload)

  **WHY Each Reference Matters**:
  - Lines 414-460 — This is the EXACT code being replaced. The prefetcher moves this work to a background thread. The main thread just calls `getNext()` and gets the same `MappedBuffers` + `MvResult`.
  - Lines 461-539 — GPU upload code stays untouched. It consumes `mappedBuffers` and `mvResult` whether they came from sequential read or prefetcher.
  - Lines 390-410 — The frame list (`sortedFrames`) is passed to `prefetcher.start()` unchanged.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with prefetcher integration
    Tool: Bash
    Preconditions: T12 committed
    Steps:
      1. cmake --build build --config Release
      2. Verify zero errors
    Expected Result: Clean build
    Failure Indicators: Compile errors (missing includes, type mismatches)
    Evidence: .sisyphus/evidence/task-13-build.txt

  Scenario: All existing integration tests still pass
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: Prefetcher produces identical output to sequential read path
    Failure Indicators: Integration test failures (output mismatch from incorrect prefetch ordering)
    Evidence: .sisyphus/evidence/task-13-all-tests.txt

  Scenario: No sequential EXR read remains in main processing path (code review)
    Tool: Bash (grep)
    Preconditions: Code committed
    Steps:
      1. Search processDirectory RR path for direct "ExrReader reader(" calls inside the frame loop
      2. Search processDirectoryFG for direct "ExrReader reader(" calls inside the frame loop
      3. Search processDirectoryRRFG for direct "ExrReader reader(" calls inside the frame loop
      4. Verify all 3 use prefetcher.getNext() instead
    Expected Result: No direct ExrReader construction inside frame loops of the 3 processing functions
    Failure Indicators: Direct ExrReader calls still present in main frame loops (prefetcher not integrated)
    Evidence: .sisyphus/evidence/task-13-prefetcher-usage.txt
  ```

  **Commit**: YES
  - Message: `perf(pipeline): integrate FramePrefetcher into processing functions`
  - Files: `src/pipeline/sequence_processor.cpp`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

- [ ] 14. Tests for FramePrefetcher

  **What to do**:
  - Create `tests/test_frame_prefetcher.cpp` with Catch2 tests:

    **Test 1: "frame_prefetcher_basic_sequence"** `[pipeline][prefetcher]`:
    - Create a temporary directory with 3 small test EXR files (use existing test fixture EXRs if available, or create minimal multi-layer EXRs using OpenEXR API):
      - Each with the required channels: `ViewLayer.Combined.RGBA`, `ViewLayer.Depth.Z`, `ViewLayer.Vector.XYZW`, etc.
      - Minimal size: 16×16 pixels
    - Create ChannelMapper and MvConverter instances
    - Create FramePrefetcher with ringSize=2
    - Call `start()` with 3 SequenceFrameInfo entries
    - Call `getNext()` 3 times
    - Verify each returns a valid PrefetchedFrame with `valid == true`
    - Verify frame ordering matches input (frameInfo.frameNumber matches expected)
    - Call `getNext()` a 4th time — should return `std::nullopt`
    - Prefetcher destructor runs cleanly

    **Test 2: "frame_prefetcher_empty_frame_list"** `[pipeline][prefetcher]`:
    - Create FramePrefetcher with ringSize=2
    - Call `start()` with empty vector
    - Call `getNext()` — should return `std::nullopt` immediately
    - No crash, no hang

    **Test 3: "frame_prefetcher_stop_before_consume"** `[pipeline][prefetcher]`:
    - Start prefetcher with 10 frames
    - Consume only 2 frames with `getNext()`
    - Call `stop()` explicitly (or let destructor run)
    - Verify no hang, no crash, worker thread joins cleanly

    **Test 4: "frame_prefetcher_invalid_exr_path"** `[pipeline][prefetcher]`:
    - Start prefetcher with 3 frames, one with a non-existent path
    - Call `getNext()` for each
    - The frame with invalid path should have `valid == false`
    - Other frames should have `valid == true`
    - No crash from background thread exception

    **Test fixture setup**: The tests need minimal EXR files. Options:
    - Use `tests/fixtures/` if they contain suitable EXRs
    - Create test EXRs programmatically using `Imf::OutputFile` (OpenEXR is already linked)
    - Use a test helper that generates MappedBuffers directly (mock approach)

    The simplest approach: If existing test fixtures in `tests/fixtures/` have valid multi-layer EXRs with required channels, use those. Otherwise, create a test helper that writes minimal EXRs to a temp directory.

  - In `tests/CMakeLists.txt`: Add `test_frame_prefetcher.cpp` to the test executable sources.

  **Must NOT do**:
  - Do NOT modify existing test files
  - Do NOT introduce test-only dependencies (use existing Catch2 + OpenEXR)
  - Do NOT use sleep-based synchronization in tests (use condition variables and getNext() blocking)
  - Do NOT create tests that flake due to timing

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Threading tests require careful synchronization verification, EXR fixture creation, and testing edge cases like early termination without deadlocks
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3 (with T3, T7, T10)
  - **Blocks**: None
  - **Blocked By**: T13 (needs prefetcher integrated for end-to-end verification)

  **References**:

  **Pattern References**:
  - `tests/test_integration.cpp:1-151` — Integration test structure: Catch2 TEST_CASE macro, section organization, REQUIRE/CHECK assertions
  - `tests/test_integration.cpp:20-50` — How test fixtures are referenced (paths, expected outputs)
  - `tests/test_texture_pipeline.cpp:10-40` — SKIP pattern for hardware-dependent tests
  - `tests/CMakeLists.txt:2-14` — How test source files are listed

  **API/Type References**:
  - `src/pipeline/frame_prefetcher.h` — FramePrefetcher API, PrefetchedFrame struct (from T12)
  - `src/pipeline/sequence_processor.h:12-15` — SequenceFrameInfo struct (test input)
  - `src/core/channel_mapper.h:41-57` — MappedBuffers struct (verify prefetched output)
  - `src/core/channel_mapper.h:63` — ChannelMapper constructor (needed to create test instance)
  - `src/core/mv_converter.h` — MvConverter constructor (needed to create test instance)

  **WHY Each Reference Matters**:
  - test_integration.cpp — Provides the Catch2 test structure pattern and how multi-file integration is tested in this project.
  - test_texture_pipeline.cpp — SKIP pattern for when hardware isn't available (though prefetcher tests are CPU-only and shouldn't need SKIP).
  - FramePrefetcher API — The exact methods being tested: `start()`, `getNext()`, `stop()`.
  - SequenceFrameInfo — Test data construction: need path + frameNumber for each test frame.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Prefetcher tests compile and run
    Tool: Bash
    Preconditions: T13 committed, build succeeds
    Steps:
      1. cmake --build build --config Release
      2. build\tests\Release\dlss-compositor-tests.exe "[prefetcher]"
      3. Verify all pass
    Expected Result: All prefetcher tests pass (no SKIP needed — these are CPU-only)
    Failure Indicators: Test failures (ordering wrong, getNext hangs, crash on stop)
    Evidence: .sisyphus/evidence/task-14-prefetcher-tests.txt

  Scenario: Empty frame list returns nullopt without hanging
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "frame_prefetcher_empty_frame_list"
      2. Verify PASS within 5 seconds (no hang)
    Expected Result: Test passes quickly
    Failure Indicators: Test hangs (deadlock in getNext with empty list)
    Evidence: .sisyphus/evidence/task-14-empty-list.txt

  Scenario: Early stop does not deadlock
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe "frame_prefetcher_stop_before_consume"
      2. Verify PASS within 10 seconds (no hang)
    Expected Result: Test passes — worker thread joins cleanly after stop
    Failure Indicators: Test hangs (deadlock — worker waiting on full ring, main thread waiting on join)
    Evidence: .sisyphus/evidence/task-14-early-stop.txt

  Scenario: All tests pass (full regression)
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. build\tests\Release\dlss-compositor-tests.exe
      2. Verify 0 failures
    Expected Result: All tests pass including new prefetcher tests
    Failure Indicators: Any failure
    Evidence: .sisyphus/evidence/task-14-all-tests.txt
  ```

  **Commit**: YES
  - Message: `test(pipeline): add tests for FramePrefetcher`
  - Files: `tests/test_frame_prefetcher.cpp`, `tests/CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release; if ($?) { build\tests\Release\dlss-compositor-tests.exe }`

---

## Final Verification Wave

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
>
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**

- [ ] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in `.sisyphus/evidence/`. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [ ] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build build --config Release` + `build\tests\Release\dlss-compositor-tests.exe`. Review all changed files for: `as any`/`@ts-ignore` (N/A for C++), empty catches, `std::cout`/`printf` debug spam in prod paths, commented-out code, unused includes. Check for C++20 features that would break MSVC C++17 mode. Check Vulkan resource leaks (every vmaCreate has matching vmaDestroy, every vkCreate has matching vkDestroy). Check thread safety (mutex around shared state, no data races).
  Output: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [ ] F3. **Real Manual QA** — `unspecified-high`
  Start from clean build. Run the test executable to verify all tests pass. Run `dlss-compositor.exe --help` to verify updated help text. If RTX GPU available: run combined mode with test fixtures, verify output frame count and resolution. Test edge cases: `--interpolate 2x` without `--scale` (FG-only), `--scale 2` without `--interpolate` (RR-only), invalid combos.
  Output: `Build [PASS/FAIL] | Tests [N/N pass] | CLI [N/N] | Integration [N/N or SKIPPED] | VERDICT`

- [ ] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (`git diff`). Verify 1:1 — everything in spec was built, nothing beyond spec was built. Check "Must NOT do" compliance per task. Verify no modifications to DlssRRProcessor, DlssFgProcessor, DlssFrameInput, DlssFgFrameInput. Detect cross-task contamination. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

| Task | Commit Message | Pre-commit Check |
|------|---------------|-----------------|
| T1   | `feat(cli): allow combined --scale + --interpolate flags` | `cmake --build build --config Release` |
| T2   | `feat(pipeline): add combined RR→FG processing mode` | build + existing tests pass |
| T3   | `test(pipeline): add integration test for combined RR+FG mode` | build + all tests pass |
| T4   | `docs: update README and usage guide for combined mode` | — |
| T5   | `perf(gpu): add batch texture upload method` | build + existing tests pass |
| T6   | `perf(pipeline): integrate batch uploads into processing functions` | build + existing tests pass |
| T7   | `test(gpu): add tests for uploadBatch` | build + all tests pass |
| T8   | `perf(gpu): add TexturePool for cross-frame VkImage reuse` | build |
| T9   | `perf(pipeline): integrate TexturePool into processing functions` | build + existing tests pass |
| T10  | `test(gpu): add tests for TexturePool` | build + all tests pass |
| T11  | `test(cli): add test for --memory-budget flag` | build + all tests pass |
| T12  | `perf(pipeline): add async FramePrefetcher with background thread` | build |
| T13  | `perf(pipeline): integrate FramePrefetcher into processing functions` | build + existing tests pass |
| T14  | `test(pipeline): add tests for FramePrefetcher` | build + all tests pass |

---

## Success Criteria

### Verification Commands
```bash
# Build
cmake --build build --config Release  # Expected: zero errors

# Run all tests
build\tests\Release\dlss-compositor-tests.exe  # Expected: all pass (45+ existing + new)

# Verify help text mentions combined mode
build\Release\dlss-compositor.exe --help  # Expected: mentions --scale + --interpolate combined

# Combined mode (requires RTX GPU + test fixtures)
build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir test_combined_out/ --scale 2 --interpolate 2x --camera-data tests/fixtures/camera.json
# Expected: success, output frames at 2x resolution, interleaved original+interpolated

# FG-only still works without RR check blocking
build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir test_fg_out/ --interpolate 2x --camera-data tests/fixtures/camera.json
# Expected: success (on RTX 40+ GPU) or proper error message
```

### Final Checklist
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
- [ ] All tests pass (existing 45 + new tests)
- [ ] Build succeeds with zero warnings on MSVC C++17
- [ ] No C++20 features used
- [ ] No modifications to DlssRRProcessor, DlssFgProcessor, DlssFrameInput, DlssFgFrameInput
- [ ] Vulkan resources properly cleaned up (no leaks)
- [ ] Thread safety verified for FramePrefetcher
