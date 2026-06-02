# DLSS Frame Generation Integration

## TL;DR

> **Quick Summary**: Add DLSS Frame Generation (2x/4x interpolation) to dlss-compositor via raw NGX Vulkan API, enabling offline Blender EXR sequences to be frame-interpolated with HDR-safe, color-preserving quality on RTX 40+ GPUs.
> 
> **Deliverables**:
> - `DlssFgProcessor` class for DLSS-G evaluation (modeled after `DlssRRProcessor`)
> - `CameraDataLoader` to parse per-frame camera.json sidecar files
> - Extended `NgxContext` with DLSS-G feature lifecycle (`m_fgFeatureHandle`)
> - `--interpolate 2x|4x` and `--camera-data` CLI flags
> - Extended `SequenceProcessor` with frame-pair interpolation loop
> - Blender `export_camera_data.py` standalone script for camera matrix export
> - Catch2 test suite for all new components
> - `nvngx_dlssg.dll` copied via CMake
> 
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 4 waves
> **Critical Path**: Task 1 (CMake+DLL) → Task 2 (NgxContext FG lifecycle) → Task 5 (DlssFgProcessor) → Task 7 (SequenceProcessor FG loop) → Task 9 (integration test) → Final Verification

---

## Context

### Original Request
Add frame interpolation (2x and 4x) to dlss-compositor for offline-rendered Blender EXR sequences. Originally RIFE was considered but rejected because it cannot handle HDR content. User wants DLSS Frame Generation which operates natively in scene-linear color space.

### Interview Summary
**Key Discussions**:
- **RIFE → DLSS-G**: RIFE can't handle HDR; DLSS-G operates in whatever color space input uses, with `colorBuffersHDR=1` for scene-linear float
- **Raw NGX over Streamline**: Streamline is designed for game engine swapchain hooks; raw NGX works on plain `VkCommandBuffer` without swapchain (confirmed by dlssg-to-fsr3 project, 4.9k stars)
- **Camera data format**: Single `camera.json` sidecar file alongside EXR sequence with per-frame entries
- **Test strategy**: Tests after implementation using existing Catch2 infrastructure
- **Hardware**: RTX 40+ for 2x FG, RTX 50+ for 4x multi-frame generation

**Research Findings**:
- All DLSS-G headers already in `DLSS/include/` (nvsdk_ngx_helpers_dlssg_vk.h, nvsdk_ngx_params_dlssg.h, nvsdk_ngx_defs_dlssg.h)
- `nvngx_dlssg.dll` already in `DLSS/lib/Windows_x86_64/rel/`
- Motion vectors: screen-space pixel units with `mvecScale = [1/W, 1/H]` normalization (different from DLSS-RR's scale=1.0)
- Camera matrices: right-handed column-major, unjittered, matches Blender conventions
- Multi-frame 4x: call evaluate 3 times with `multiFrameIndex=1,2,3` on same backbuffer
- DLSS-G computes optical flow internally — no external OF computation needed

### Metis Review
**Identified Gaps** (addressed):
- **NgxContext architecture**: Add `m_fgFeatureHandle` alongside existing `m_featureHandle` (Option A — minimal disruption)
- **NativeBackbufferFormat**: Needs investigation — VkFormat value vs DXGI enum. Noted as validation item in Task 2.
- **Frame pair buffering**: Keep previous frame textures alive across loop iterations (simpler, acceptable VRAM)
- **Output file naming**: Re-number entire sequence (original_0001→0001, interp→0002, original_0002→0003, etc.) for direct video encoding
- **Standalone FG only**: No pipeline mode (RR → FG chaining) in this iteration
- **Depth convention**: Blender Z pass is linear distance; DLSS-G has `depthInverted` flag — validate in feasibility test
- **clipToPrevClip derivation**: Computed at runtime from consecutive frame camera data, not pre-computed in camera.json
- **Edge cases**: First frame gets `reset=true`, sequence gaps trigger reset, single-frame input errors early

---

## Work Objectives

### Core Objective
Enable dlss-compositor to interpolate between consecutive EXR frames using NVIDIA DLSS Frame Generation, producing 2x (RTX 40+) or 4x (RTX 50+) frame rate output while preserving HDR scene-linear color fidelity.

### Concrete Deliverables
- `src/dlss/dlss_fg_processor.h/.cpp` — Frame Generation evaluation class
- `src/core/camera_data_loader.h/.cpp` — Camera JSON parser with matrix derivation
- Extended `src/dlss/ngx_wrapper.h/.cpp` — DLSS-G feature lifecycle
- Extended `src/cli/config.h` + `src/cli/cli_parser.cpp` — New CLI flags
- Extended `src/pipeline/sequence_processor.cpp` — Frame-pair interpolation loop
- `blender/export_camera_data.py` — Standalone Blender camera export script
- `tests/test_camera_data_loader.cpp` — Camera loader unit tests
- `tests/test_dlss_fg_processor.cpp` — FG processor integration tests
- Extended `tests/test_cli.cpp` — CLI flag parsing tests
- Updated `CMakeLists.txt` + `tests/CMakeLists.txt` — DLL copying + new source files

### Definition of Done
- [ ] `dlss-compositor.exe --input-dir <dir> --output-dir <out> --interpolate 2x --camera-data camera.json` produces interpolated EXR sequence on RTX 40+ GPU
- [ ] Interpolated frames are non-zero, R16G16B16A16_SFLOAT, and contain HDR values (not clamped to [0,1])
- [ ] `--interpolate 4x` produces 4x frames on RTX 50+ or gracefully errors/falls back on RTX 40
- [ ] All Catch2 tests pass: `dlss-compositor-tests`
- [ ] Build succeeds with no warnings: `cmake --build build --config Release`

### Must Have
- DLSS Frame Generation 2x interpolation via raw NGX Vulkan API
- DLSS 4 Multi-Frame Generation 4x support (RTX 50)
- Per-frame camera matrix input via JSON sidecar
- `colorBuffersHDR=1` for scene-linear HDR preservation
- Graceful hardware detection (RTX 40+ required, clear error on incompatible GPU)
- `reset=true` on first frame and after sequence gaps
- Catch2 tests for camera loader, CLI flags, and FG processor

### Must NOT Have (Guardrails)
- NO Streamline SDK dependency — raw NGX API only
- NO RIFE integration — DLSS-G replaces it entirely
- NO tonemap/inverse-tonemap pipeline — DLSS-G works in scene-linear
- NO pipeline mode (RR → FG chaining) — standalone FG only for this iteration
- NO GUI/viewer integration — CLI-only for frame generation
- NO dynamic resolution scaling — all frames same resolution
- NO orthographic camera support — perspective only
- NO HUDless, UI, or BidirectionalDistortionField inputs
- NO ACES2/OCIO integration
- NO modifications to existing DlssRRProcessor or DlssFrameInput

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — ALL verification is agent-executed. No exceptions.

### Test Decision
- **Infrastructure exists**: YES (Catch2, 9 existing test files)
- **Automated tests**: Tests-after implementation
- **Framework**: Catch2 (existing)
- **If TDD**: N/A — tests after implementation

### QA Policy
Every task MUST include agent-executed QA scenarios.
Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **Build verification**: Bash — cmake build, check exit code and warnings
- **CLI testing**: Bash — run exe with flags, check stdout/stderr/exit code
- **Unit tests**: Bash — `dlss-compositor-tests "[tag]"` with output capture
- **GPU integration**: Bash — run exe with test fixtures, validate output EXR with Python script
- **Blender script**: Bash — `blender --background --python export_camera_data.py -- --test`

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately — foundation, no dependencies):
├── Task 1: CMake: copy nvngx_dlssg.dll + add new source files [quick]
├── Task 3: CameraDataLoader — JSON parser with matrix derivation [unspecified-high]
├── Task 4: CLI flags — --interpolate and --camera-data [quick]
├── Task 6: Blender export_camera_data.py script [unspecified-high]
└── (4 parallel tasks)

Wave 2 (After Task 1 — NGX lifecycle, needs DLL):
├── Task 2: NgxContext DLSS-G lifecycle — availability check + feature creation [deep]
└── (1 task — feasibility gate, blocks all GPU work)

Wave 3 (After Tasks 2+3+4 — core implementation):
├── Task 5: DlssFgProcessor — frame generation evaluation class [deep]
├── Task 7: SequenceProcessor FG loop — frame-pair processing + output naming [deep]
└── (2 tasks, can run in parallel if Task 5 finishes quickly; Task 7 depends on 5)

Wave 4 (After all implementation — tests + integration):
├── Task 8: Catch2 test suite — camera loader, CLI, FG processor tests [unspecified-high]
├── Task 9: Integration test — end-to-end 2-frame interpolation [deep]
└── (2 parallel tasks)

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real manual QA (unspecified-high)
└── Task F4: Scope fidelity check (deep)
→ Present results → Get explicit user okay
```

### Critical Path
Task 1 → Task 2 (feasibility gate) → Task 5 → Task 7 → Task 9 → F1-F4 → user okay

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | - | 2, 5, 7, 8, 9 | 1 |
| 2 | 1 | 5, 7, 8, 9 | 2 |
| 3 | - | 5, 7, 8, 9 | 1 |
| 4 | - | 7, 8 | 1 |
| 5 | 1, 2, 3 | 7, 8, 9 | 3 |
| 6 | - | 9 | 1 |
| 7 | 3, 4, 5 | 8, 9 | 3 |
| 8 | 2, 3, 4, 5, 7 | 9 | 4 |
| 9 | all | FINAL | 4 |

### Agent Dispatch Summary

- **Wave 1**: **4 tasks** — T1 → `quick`, T3 → `unspecified-high`, T4 → `quick`, T6 → `unspecified-high`
- **Wave 2**: **1 task** — T2 → `deep`
- **Wave 3**: **2 tasks** — T5 → `deep`, T7 → `deep`
- **Wave 4**: **2 tasks** — T8 → `unspecified-high`, T9 → `deep`
- **FINAL**: **4 tasks** — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. CMake: Copy nvngx_dlssg.dll + Register New Source Files

  **What to do**:
  - Add `nvngx_dlssg.dll` copy command to main `CMakeLists.txt` following the exact pattern of `nvngx_dlssd.dll` copy at lines 156-163
  - Add `nvngx_dlssg.dll` copy to `tests/CMakeLists.txt` following the pattern at lines 34-40
  - Register new source files (to be created by later tasks): `src/dlss/dlss_fg_processor.cpp`, `src/core/camera_data_loader.cpp`
  - Add new test source files: `tests/test_camera_data_loader.cpp`, `tests/test_dlss_fg_processor.cpp`
  - Verify build still succeeds (new source files won't exist yet — add them conditionally or as empty stubs)

  **Must NOT do**:
  - Do NOT modify any existing source file compilation settings
  - Do NOT add Streamline SDK or any new external dependency
  - Do NOT change the existing nvngx_dlssd.dll copy logic

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Small CMake-only change following existing patterns exactly
  - **Skills**: []
    - No special skills needed — pure CMake editing

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 3, 4, 6)
  - **Blocks**: Tasks 2, 5, 7, 8, 9 (all need DLL present to build/run)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `CMakeLists.txt:156-163` — Exact pattern for copying `nvngx_dlssd.dll` to main target output dir. Replicate this for `nvngx_dlssg.dll` alongside it.
  - `tests/CMakeLists.txt:34-40` — Exact pattern for copying DLLs to test target output dir. Add `nvngx_dlssg.dll` to the same loop/glob.

  **API/Type References**:
  - `DLSS/lib/Windows_x86_64/rel/nvngx_dlssg.dll` — The source DLL to be copied (verify it exists at this path)
  - `DLSS/lib/Windows_x86_64/dev/nvngx_dlssg.dll` — Dev version also exists; follow whichever convention the existing RR copy uses

  **WHY Each Reference Matters**:
  - `CMakeLists.txt:156-163`: This is the EXACT boilerplate to copy — same `add_custom_command` pattern, same `$<TARGET_FILE_DIR:...>` target directory. Just change the filename from `nvngx_dlssd.dll` to `nvngx_dlssg.dll`.
  - `tests/CMakeLists.txt:34-40`: Tests need the DLL too or they'll fail at runtime with missing DLL error.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds after CMake changes
    Tool: Bash
    Preconditions: Clean build directory exists
    Steps:
      1. Run `cmake --build build --config Release 2>&1`
      2. Check exit code is 0
      3. Check output does not contain "error"
    Expected Result: Build succeeds with exit code 0
    Failure Indicators: Non-zero exit code, "error" in build output
    Evidence: .sisyphus/evidence/task-1-build-success.txt

  Scenario: nvngx_dlssg.dll present in build output
    Tool: Bash
    Preconditions: Build completed successfully
    Steps:
      1. Run `dir build\Release\nvngx_dlssg.dll` (or equivalent path)
      2. Verify file exists and is non-zero size
      3. Run `dir build\Release\nvngx_dlssd.dll` to confirm existing DLL still present
    Expected Result: Both nvngx_dlssg.dll and nvngx_dlssd.dll present in build output
    Failure Indicators: File not found, zero-size file
    Evidence: .sisyphus/evidence/task-1-dll-present.txt
  ```

  **Commit**: YES
  - Message: `build: add nvngx_dlssg.dll copy to build output`
  - Files: `CMakeLists.txt`, `tests/CMakeLists.txt`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 2. NgxContext DLSS-G Feature Lifecycle — Availability Check + Feature Creation

  **What to do**:
  - Add `m_fgFeatureHandle` member to `NgxContext` class (alongside existing `m_featureHandle`)
  - Add `bool m_dlssFGAvailable = false;` member
  - Add `isDlssFGAvailable()` public method — queries `NVSDK_NGX_Parameter_FrameGeneration_Available` capability
  - Add `queryDlssFGAvailability()` private method — called during `init()`, queries capability and multi-frame max
  - Add `createDlssFG(width, height, backbufferFormat, cmdBuf, errorMsg)` public method:
    - Fill `NVSDK_NGX_DLSSG_Create_Params` struct (Width, Height, NativeBackbufferFormat=VK_FORMAT_R16G16B16A16_SFLOAT cast to unsigned int, DynamicResolutionScaling=false)
    - Call `NGX_VK_CREATE_DLSSG(cmdBuf, 1, 1, &m_fgFeatureHandle, m_parameters, &createParams)`
    - Handle errors with descriptive messages (especially "RTX 40+ required" on capability miss)
  - Add `releaseDlssFG()` public method — calls `NVSDK_NGX_VULKAN_ReleaseFeature(m_fgFeatureHandle)`
  - Add `fgFeatureHandle()` getter
  - Add `maxMultiFrameCount()` method — reads `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` from parameters
  - Update `shutdown()` to also release FG handle if active
  - Add `#include "nvsdk_ngx_helpers_dlssg_vk.h"` to ngx_wrapper.cpp (NOT the header — keep header clean with forward decls)

  **CRITICAL**: This is the **feasibility gate**. If `NGX_VK_CREATE_DLSSG` fails in headless mode (no swapchain), the entire DLSS-G approach is blocked. Test this FIRST before investing in other tasks.

  **Investigation needed**: `NativeBackbufferFormat` — the `NVSDK_NGX_DLSSG_Create_Params::NativeBackbufferFormat` field is `unsigned int`. Test whether passing `VK_FORMAT_R16G16B16A16_SFLOAT` (=97) directly works, or if DXGI_FORMAT_R16G16B16A16_FLOAT (=10) is expected. Try VkFormat first since we're using the Vulkan API path.

  **Must NOT do**:
  - Do NOT modify `m_featureHandle` or any existing DLSS-RR methods
  - Do NOT add Streamline SDK headers or dependencies
  - Do NOT create the DlssFgProcessor class in this task (that's Task 5)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Feasibility gate requiring careful NGX API integration, error handling, and format investigation
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (sequential after Wave 1)
  - **Blocks**: Tasks 5, 7, 8, 9
  - **Blocked By**: Task 1 (needs nvngx_dlssg.dll in build output)

  **References**:

  **Pattern References**:
  - `src/dlss/ngx_wrapper.h:19-60` — Full NgxContext class. Add new members alongside existing ones. Follow the exact pattern: public method → private helper → private member.
  - `src/dlss/ngx_wrapper.cpp:140-198` (read the `createDlssRR` method) — Exact pattern for feature creation: fill params struct → set NGX parameters → call CreateFeature → check result → return success/error message. Replicate for DLSS-G.
  - `src/dlss/ngx_wrapper.cpp:1-40` (init method) — Where `queryDlssRRAvailability()` is called. Add `queryDlssFGAvailability()` call alongside it.

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h:45-60` — `NGX_VK_CREATE_DLSSG()` function signature and parameter setup. This is the function to call.
  - `DLSS/include/nvsdk_ngx_params_dlssg.h:31-39` — `NVSDK_NGX_DLSSG_Create_Params` struct definition (Width, Height, NativeBackbufferFormat, RenderWidth, RenderHeight, DynamicResolutionScaling)
  - `DLSS/include/nvsdk_ngx_defs_dlssg.h:32-36` — `NVSDK_NGX_Parameter_FrameGeneration_Available` capability query constant
  - `DLSS/include/nvsdk_ngx_defs_dlssg.h:260-271` — DLSS 4 multi-frame constants: `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` (max generated frames, e.g. 3 for 4x), `NVSDK_NGX_DLSSG_Parameter_MultiFrameCount`, `NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex`

  **External References**:
  - `DLSS/doc/DLSS-FG Programming Guide.pdf` — Official integration guide for feature creation workflow

  **WHY Each Reference Matters**:
  - `ngx_wrapper.cpp:140-198`: This is the template to follow exactly — same error handling pattern, same parameter setup flow, same return convention. The new method should look structurally identical but with DLSS-G parameters.
  - `nvsdk_ngx_helpers_dlssg_vk.h:45-60`: The helper function handles all parameter-setting boilerplate for creation. Don't manually set parameters — use this helper.
  - `nvsdk_ngx_defs_dlssg.h:32-36`: `FrameGeneration_Available` capability constant for GPU support check. Lines 260-271 contain the DLSS 4 multi-frame constants (`MultiFrameCountMax`, `MultiFrameCount`, `MultiFrameIndex`) needed for 4x detection and evaluation.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: DLSS-G availability check on RTX 40+ GPU
    Tool: Bash
    Preconditions: Build succeeds with nvngx_dlssg.dll present, RTX 40+ GPU installed
    Steps:
      1. Build and run a minimal test that calls NgxContext::init() then NgxContext::isDlssFGAvailable()
      2. Check return value is true on RTX 40+ hardware
      3. Check maxMultiFrameCount() returns >= 1
    Expected Result: isDlssFGAvailable() returns true, maxMultiFrameCount() >= 1
    Failure Indicators: Returns false on RTX 40+ GPU, throws exception, crash
    Evidence: .sisyphus/evidence/task-2-fg-availability.txt

  Scenario: DLSS-G feature creation succeeds headless (no swapchain)
    Tool: Bash
    Preconditions: isDlssFGAvailable() returns true
    Steps:
      1. Call createDlssFG(1920, 1080, VK_FORMAT_R16G16B16A16_SFLOAT, cmdBuf, errorMsg)
      2. Verify return value is true
      3. Verify fgFeatureHandle() is non-null
      4. Call releaseDlssFG() — verify no crash
    Expected Result: Feature creation succeeds, handle is valid, release is clean
    Failure Indicators: Returns false with "swapchain" or "present" in error message, null handle, crash on release
    Evidence: .sisyphus/evidence/task-2-fg-creation.txt

  Scenario: Graceful error on non-RTX-40 GPU (if testable)
    Tool: Bash
    Preconditions: Non-RTX-40 GPU or mock unavailability
    Steps:
      1. Call isDlssFGAvailable() — should return false
      2. Call createDlssFG() — should return false with clear error message
      3. Verify error message contains "RTX 40" or "not supported"
    Expected Result: Clean error, no crash, descriptive message
    Failure Indicators: Crash, generic error message, undefined behavior
    Evidence: .sisyphus/evidence/task-2-fg-unavailable.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): add DLSS-G feature lifecycle to NgxContext`
  - Files: `src/dlss/ngx_wrapper.h`, `src/dlss/ngx_wrapper.cpp`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 3. CameraDataLoader — JSON Parser with Matrix Derivation

  **What to do**:
  - Create `src/core/camera_data_loader.h` and `src/core/camera_data_loader.cpp`
  - Define `CameraFrameData` struct containing per-frame camera info:
    - `float matrix_world[4][4]` — Blender's camera world matrix (4x4)
    - `float projection[4][4]` — Blender's projection matrix (4x4)
    - `float fov` — Field of view in radians
    - `float aspect_ratio` — width/height
    - `float near_clip`, `float far_clip` — clipping planes
    - `float position[3]` — camera world position
    - `float up[3]`, `float right[3]`, `float forward[3]` — camera basis vectors
  - Define `DlssFgCameraParams` struct (derived from two consecutive CameraFrameData):
    - `float cameraViewToClip[4][4]` — current frame's view-to-clip (projection matrix)
    - `float clipToCameraView[4][4]` — inverse of above
    - `float clipToLensClip[4][4]` — identity matrix (standard pinhole projection)
    - `float clipToPrevClip[4][4]` — computed: `currentClip → world → prevClip`
    - `float prevClipToClip[4][4]` — inverse of clipToPrevClip
    - Camera position, basis vectors, near, far, FOV, aspect ratio
  - Implement `CameraDataLoader` class:
    - `bool load(const std::string& jsonPath, std::string& errorMsg)` — parse camera.json, validate all required fields
    - `bool hasFrame(int frameNumber) const` — check if frame data exists
    - `const CameraFrameData& getFrame(int frameNumber) const` — get raw frame data
    - `DlssFgCameraParams computePairParams(int prevFrame, int currFrame) const` — compute derived matrices from two consecutive frames
  - Implement 4x4 matrix utility functions (multiply, inverse, identity) — either inline in the .cpp or in a small `mat4_utils.h` header
  - JSON parsing: use nlohmann/json (check if already in project) or a simple JSON parser. If not available, use a minimal header-only parser or hand-roll for this simple schema.
  - Camera.json schema:
    ```json
    {
      "frames": {
        "0001": {
          "matrix_world": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
          "projection": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
          "fov": 0.6911,
          "aspect_ratio": 1.7778,
          "near_clip": 0.1,
          "far_clip": 100.0
        }
      }
    }
    ```
  - Extract camera position from `matrix_world` column 3 (translation)
  - Extract basis vectors from `matrix_world` columns 0 (right), 1 (up), 2 (forward)
  - Compute `clipToPrevClip = prevProjection * prevViewInv * currentView * currentProjectionInv` where view = inverse of matrix_world

  **Must NOT do**:
  - Do NOT embed this in NgxContext or DlssRRProcessor — standalone module
  - Do NOT use heavy JSON libraries (boost::json, rapidjson) if nlohmann/json is available
  - Do NOT pre-compute clipToPrevClip in the JSON file — compute at runtime

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Pure CPU implementation with matrix math, JSON parsing, and error handling — moderately complex
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 4, 6)
  - **Blocks**: Tasks 5, 7, 8, 9
  - **Blocked By**: None (pure CPU, no GPU dependency)

  **References**:

  **Pattern References**:
  - `src/core/exr_reader.h` — Follow naming convention and class structure pattern (constructor, load method, getters)
  - `src/core/channel_mapper.h` — Another example of a loader/parser class in the project

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_params_dlssg.h:41-129` — `NVSDK_NGX_DLSSG_Opt_Eval_Params` struct — this is what the output `DlssFgCameraParams` must populate. Match all field names and types exactly.

  **External References**:
  - nlohmann/json: Check `CMakeLists.txt` for existing JSON dependency. If not present, use FetchContent to add it (consistent with project's dependency management).

  **WHY Each Reference Matters**:
  - `nvsdk_ngx_params_dlssg.h:41-129`: The `DlssFgCameraParams` struct must produce values that directly map into `NVSDK_NGX_DLSSG_Opt_Eval_Params` fields. Cross-reference field names and array sizes carefully (e.g., `float[4][4]` not `float[16]`).
  - `exr_reader.h`: Establishes the project's convention for loader classes — constructor, load(), getters, error message pattern.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

   ```
  Scenario: Parse valid camera JSON with 2 frames
    Tool: Bash
    Preconditions: Build succeeds with camera_data_loader compiled in
    Steps:
      1. Create a temporary test JSON file (write to build/test_camera.json) with 2 frame entries:
         frame "0001" at position (0,0,5) looking at origin, FOV=0.6911, near=0.1, far=100
         frame "0002" at position (1,0,5) looking at origin (camera moved 1 unit right)
      2. Write a minimal C++ test program (or use existing Catch2 test runner) that:
         a. Calls CameraDataLoader::load("build/test_camera.json", errorMsg)
         b. Asserts load returns true
         c. Asserts hasFrame(1) returns true
         d. Asserts getFrame(1).fov ≈ 0.6911 (±0.001)
         e. Asserts getFrame(1).matrix_world is not all zeros
      3. Build and run the test program
    Expected Result: All assertions pass — load succeeds, FOV matches, matrices populated
    Failure Indicators: Parse error, wrong values, crash, load returns false
    Evidence: .sisyphus/evidence/task-3-camera-parse.txt

  Scenario: Reject malformed camera JSON
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. Create a temporary JSON file with malformed content (missing "frames" key, or invalid JSON syntax)
      2. Call CameraDataLoader::load() on the malformed file
      3. Verify load() returns false
      4. Verify errorMsg is non-empty and describes the issue
    Expected Result: Returns false, error message describes the issue
    Failure Indicators: Returns true on invalid JSON, crash, empty error
    Evidence: .sisyphus/evidence/task-3-camera-invalid.txt

  Scenario: computePairParams produces valid clipToPrevClip
    Tool: Bash
    Preconditions: The 2-frame test JSON from scenario 1 is available
    Steps:
      1. Load the 2-frame camera JSON
      2. Call computePairParams(1, 2)
      3. Verify clipToPrevClip is not identity (cameras moved between frames)
      4. Verify clipToLensClip IS identity (standard pinhole, no lens distortion)
      5. Verify position extracted from frame 1 ≈ (0, 0, 5)
    Expected Result: Derived matrices are non-identity, clipToLensClip is identity, position correct
    Failure Indicators: All matrices identity, NaN values, wrong position
    Evidence: .sisyphus/evidence/task-3-camera-derived.txt
  ```

  > **NOTE**: Task 3 QA creates its own temporary JSON fixtures inline — it does NOT depend on `tests/fixtures/camera.json` (which is created later in Task 8 for the permanent test suite).

  **Commit**: YES
  - Message: `feat(core): add CameraDataLoader for per-frame camera JSON parsing`
  - Files: `src/core/camera_data_loader.h`, `src/core/camera_data_loader.cpp`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 4. CLI Flags — `--interpolate` and `--camera-data`

  **What to do**:
  - Add to `AppConfig` in `config.h`:
    - `int interpolateFactor = 0;` — 0 = disabled, 2 = 2x, 4 = 4x
    - `std::string cameraDataFile;` — path to camera.json
  - Add parsing in `cli_parser.cpp`:
    - `--interpolate <value>` where value is `2x` or `4x` (case-insensitive). Parse to int 2 or 4. Error on invalid values.
    - `--camera-data <path>` — store path string
  - Add validation:
    - If `--interpolate` is specified, `--camera-data` must also be specified (error otherwise)
    - If `--interpolate` value is not `2x` or `4x`, print error and exit
    - `--interpolate` without a value prints error
  - Update help text to document new flags

  **Must NOT do**:
  - Do NOT add `--interpolate` logic to SequenceProcessor (that's Task 7)
  - Do NOT add camera.json loading logic (that's Task 3)
  - Do NOT modify existing flag parsing

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple CLI flag addition following existing parser patterns
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 3, 6)
  - **Blocks**: Tasks 7, 8
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:43-60` — `parseQuality()` function: exact pattern for parsing enum-like string values ("MaxQuality", "Balanced", etc.). Follow this for parsing "2x"/"4x".
  - `src/cli/cli_parser.cpp:61-320` — Main parsing loop with `isFlag()` checks and value extraction. Add new flags following the same if/else chain pattern.
  - `src/cli/config.h:35-60` — `AppConfig` struct. Add new fields at the bottom, before the closing brace.

  **WHY Each Reference Matters**:
  - `cli_parser.cpp:43-60`: The `parseQuality()` pattern shows how to parse string enum values with strcmp. Write a similar `parseInterpolateFactor(const char* value, int& out)` function.
  - `config.h:35-60`: Must add fields here that are consistent with existing naming and type conventions (e.g., `bool encodeVideo` pattern for feature flags).

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Parse --interpolate 2x flag
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. Run `dlss-compositor-tests "[cli]"` to run CLI parsing tests
      2. Verify test for --interpolate 2x parses interpolateFactor=2
      3. Verify test for --interpolate 4x parses interpolateFactor=4
    Expected Result: Both values parse correctly
    Failure Indicators: Wrong factor value, parse error
    Evidence: .sisyphus/evidence/task-4-cli-parse.txt

  Scenario: Error on --interpolate without --camera-data
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. Run `dlss-compositor.exe --input-dir renders --output-dir out --interpolate 2x` (no --camera-data)
      2. Check exit code is non-zero
      3. Check stderr contains "camera-data" or "camera"
    Expected Result: Non-zero exit, error message about missing camera data
    Failure Indicators: Exit 0, no error message, crash
    Evidence: .sisyphus/evidence/task-4-cli-error.txt

  Scenario: Error on invalid --interpolate value
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. Run `dlss-compositor.exe --interpolate 3x`
      2. Check exit code is non-zero
      3. Check stderr mentions valid values (2x, 4x)
    Expected Result: Non-zero exit, helpful error message
    Failure Indicators: Exit 0, crash, unhelpful error
    Evidence: .sisyphus/evidence/task-4-cli-invalid.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add --interpolate and --camera-data flags`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 5. DlssFgProcessor — Frame Generation Evaluation Class

  **What to do**:
  - Create `src/dlss/dlss_fg_processor.h` with:
    - `DlssFgFrameInput` struct:
      - `VkImage backbuffer`, `VkImageView backbufferView` — current frame color (DLSS-RR output or raw EXR)
      - `VkImage depth`, `VkImageView depthView` — current frame depth
      - `VkImage motionVectors`, `VkImageView motionView` — current frame motion vectors
      - `VkImage outputInterp`, `VkImageView outputInterpView` — output interpolated frame
      - `uint32_t width`, `uint32_t height` — frame dimensions
      - `bool reset` — true for first frame or after sequence gap
      - `DlssFgCameraParams cameraParams` — derived camera parameters from CameraDataLoader
      - `unsigned int multiFrameCount` — 1 for 2x, 3 for 4x
      - `unsigned int multiFrameIndex` — 1..multiFrameCount
    - `DlssFgProcessor` class:
      - Constructor takes `VulkanContext&` and `NgxContext&` (same pattern as DlssRRProcessor)
      - `bool evaluate(VkCommandBuffer cmdBuf, const DlssFgFrameInput& frame, std::string& errorMsg)`
  - Create `src/dlss/dlss_fg_processor.cpp` implementing:
    - `evaluate()` method:
      1. Wrap VkImage/VkImageView into `NVSDK_NGX_Resource_VK` using `NVSDK_NGX_Create_ImageView_Resource_VK()` — follow exact pattern from `dlss_rr_processor.cpp:62-85` (`makeImageResource`)
      2. Fill `NVSDK_NGX_VK_DLSSG_Eval_Params` struct:
         - `pBackbuffer` = wrapped backbuffer resource
         - `pDepth` = wrapped depth resource
         - `pMVecs` = wrapped motion vectors resource
         - `pOutputInterpFrame` = wrapped output resource
         - `pHudless = nullptr`, `pUI = nullptr`, etc. (all optional = nullptr)
      3. Fill `NVSDK_NGX_DLSSG_Opt_Eval_Params` struct from `DlssFgCameraParams`:
         - Copy all camera matrices (cameraViewToClip, clipToCameraView, clipToLensClip, clipToPrevClip, prevClipToClip)
         - Set `mvecScale[0] = 1.0f / width`, `mvecScale[1] = 1.0f / height` — **NOT** scale=1.0 like DLSS-RR!
         - Set `colorBuffersHDR = true`
         - Set `depthInverted = false` (Blender linear depth, closer = smaller value)
         - Set `cameraMotionIncluded = true` (Blender Vector pass includes camera motion)
         - Set `orthoProjection = false`
         - Set `reset` from input
         - Set `notRenderingGameFrames = false`
         - Set `jitterOffset = {0, 0}` (no TAA jitter in offline renders)
         - Set `cameraPinholeOffset = {0, 0}`
         - Set `multiFrameCount` and `multiFrameIndex` from input
         - Set `motionVectorsInvalidValue = 0.0f`
         - Set `motionVectorsDilated = false`
         - Set `menuDetectionEnabled = false`
         - Set all subrect bases to {0,0} and sizes to {width, height} or {0,0} to use full buffer
      4. Call `NGX_VK_EVALUATE_DLSSG(cmdBuf, m_ngx.fgFeatureHandle(), m_ngx.parameters(), &evalParams, &optEvalParams)`
      5. Check result, return error message on failure
  - Include `nvsdk_ngx_helpers_dlssg_vk.h` in the .cpp file

  **Must NOT do**:
  - Do NOT modify DlssRRProcessor or DlssFrameInput
  - Do NOT add swapchain/present logic
  - Do NOT add frame pacing (irrelevant for offline)
  - Do NOT set mvecScale to 1.0 — DLSS-G normalizes to [-1,1], not pixel units

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Core GPU integration requiring precise NGX API usage, careful parameter setup, and format handling
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Tasks 1, 2, 3)
  - **Parallel Group**: Wave 3
  - **Blocks**: Tasks 7, 8, 9
  - **Blocked By**: Tasks 1 (DLL), 2 (NgxContext FG methods), 3 (CameraDataLoader for DlssFgCameraParams type)

  **References**:

  **Pattern References**:
  - `src/dlss/dlss_rr_processor.h:14-40` — `DlssFrameInput` struct. Model `DlssFgFrameInput` after this but with DLSS-G specific fields (camera params instead of albedo/normals/roughness).
  - `src/dlss/dlss_rr_processor.cpp:62-85` — `makeImageResource()` helper. Copy this function into dlss_fg_processor.cpp (or extract to shared utility). It wraps VkImage+VkImageView into NVSDK_NGX_Resource_VK.
  - `src/dlss/dlss_rr_processor.cpp:87-160` — `evaluate()` method. Follow the same structure: wrap resources → fill params → call NGX evaluate → check result.
  - `src/dlss/dlss_rr_processor.h:42-51` — Class structure with VulkanContext& and NgxContext& references.

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h:31-43` — `NVSDK_NGX_VK_DLSSG_Eval_Params` struct (the required inputs to fill)
  - `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h:62-178` — `NGX_VK_EVALUATE_DLSSG()` function — reads through ALL the parameter-setting code to understand what each field maps to
  - `DLSS/include/nvsdk_ngx_params_dlssg.h:41-129` — `NVSDK_NGX_DLSSG_Opt_Eval_Params` struct — all camera matrix fields, flags, and their default values

  **WHY Each Reference Matters**:
  - `dlss_rr_processor.cpp:62-85` (makeImageResource): This is the proven pattern for wrapping Vulkan resources for NGX. Re-use it exactly — the VkImageSubresourceRange, ReadWrite flags, and format handling are critical.
  - `nvsdk_ngx_helpers_dlssg_vk.h:62-178`: Read this ENTIRE function to understand all parameters being set. The helper function IS the documentation — it shows exactly which NGX parameters map to which struct fields.
  - `nvsdk_ngx_params_dlssg.h:64-65`: `mvecScale` comment says "normalize to [-1,1] range" — this is the critical difference from DLSS-RR where scale=1.0 means pixel units.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: DlssFgProcessor evaluate succeeds on 2-frame pair
    Tool: Bash (integration test)
    Preconditions: NgxContext init + createDlssFG succeeded, two frame textures uploaded to GPU
    Steps:
      1. Create DlssFgFrameInput with backbuffer/depth/mvec from frame N, camera params from frames N and N+1
      2. Set reset=true (first evaluation), multiFrameCount=1, multiFrameIndex=1
      3. Call evaluate(cmdBuf, input, errorMsg)
      4. Verify return value is true
      5. Download outputInterp texture — verify non-zero pixels
    Expected Result: evaluate() returns true, output has non-zero pixel data
    Failure Indicators: Returns false, error message, output is all zeros, crash
    Evidence: .sisyphus/evidence/task-5-fg-evaluate.txt

  Scenario: DlssFgProcessor with incorrect mvecScale (regression guard)
    Tool: Bash (unit test verification)
    Preconditions: Code compiled
    Steps:
      1. Inspect dlss_fg_processor.cpp source code
      2. Verify mvecScale[0] = 1.0f/width (NOT 1.0f)
      3. Verify mvecScale[1] = 1.0f/height (NOT 1.0f)
    Expected Result: mvecScale uses normalization factor, not pixel-unit scale
    Failure Indicators: mvecScale set to 1.0f (DLSS-RR convention — WRONG for DLSS-G)
    Evidence: .sisyphus/evidence/task-5-mvec-scale.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): add DlssFgProcessor for DLSS Frame Generation evaluation`
  - Files: `src/dlss/dlss_fg_processor.h`, `src/dlss/dlss_fg_processor.cpp`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 6. Blender `export_camera_data.py` Standalone Script

  **What to do**:
  - Create `blender/export_camera_data.py` — standalone Blender Python script (NOT integrated into aov_export_preset.py addon)
  - Script usage: `blender --background <file.blend> --python export_camera_data.py -- --output camera.json [--start 1] [--end 250]`
  - For each frame in range:
    1. Set `bpy.context.scene.frame_set(frame_num)`
    2. Get active camera: `cam = bpy.context.scene.camera`
    3. Extract `matrix_world`: `cam.matrix_world` — convert to list of 4 lists of 4 floats (column-major)
    4. Compute projection matrix: `cam.calc_matrix_camera(bpy.context.evaluated_depsgraph_get(), x=render_x, y=render_y)` — or manually from FOV/aspect
    5. Extract `fov`: `cam.data.angle` (already in radians)
    6. Extract `aspect_ratio`: `scene.render.resolution_x / scene.render.resolution_y`
    7. Extract `near_clip`: `cam.data.clip_start`
    8. Extract `far_clip`: `cam.data.clip_end`
  - Output JSON format matching CameraDataLoader schema:
    ```json
    {
      "version": 1,
      "render_width": 1920,
      "render_height": 1080,
      "frames": {
        "0001": { "matrix_world": [...], "projection": [...], "fov": 0.6911, "aspect_ratio": 1.7778, "near_clip": 0.1, "far_clip": 100.0 },
        "0002": { ... }
      }
    }
    ```
  - Frame number keys: zero-padded to match EXR naming (e.g., "0001", "0002")
  - Add `--test` flag that validates the script can import and produce valid output structure
  - Add error handling for: no active camera, no frames in range

  **Must NOT do**:
  - Do NOT modify `blender/aov_export_preset.py` — this is a separate standalone script
  - Do NOT add Blender UI panels or operators
  - Do NOT add non-standard Python dependencies (json, math are stdlib)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Blender Python API knowledge needed, matrix extraction requires understanding Blender's coordinate system
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 3, 4)
  - **Blocks**: Task 9 (integration test needs camera.json fixtures)
  - **Blocked By**: None (fully independent)

  **References**:

  **Pattern References**:
  - `blender/aov_export_preset.py` — Existing Blender Python script in the project. Follow the same coding style (imports, error handling, argument parsing pattern).

  **API/Type References**:
  - `src/core/camera_data_loader.h` (to be created in Task 3) — The JSON schema consumed by CameraDataLoader. The Python script must produce JSON matching this exact schema.
  - `DLSS/include/nvsdk_ngx_params_dlssg.h:41-129` — Ultimate consumer of this data. Matrix convention (float[4][4], column-major) must match.

  **External References**:
  - Blender Python API: `bpy.types.Camera.angle` (FOV in radians), `bpy.types.Object.matrix_world` (4x4 Matrix), `bpy.types.Camera.calc_matrix_camera()` (projection matrix)

  **WHY Each Reference Matters**:
  - `aov_export_preset.py`: Establishes project conventions for Blender scripts — import style, argument parsing via `--` separator, error reporting.
  - The JSON schema from Task 3: The Python output must be byte-for-byte compatible with the C++ parser. Use same key names ("matrix_world", "projection", "fov", etc.).
  - Matrix convention: Blender stores matrices column-major internally, but `Matrix.to_list()` returns row-major. Need to verify and possibly transpose.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Script produces valid camera.json from factory-startup scene
    Tool: Bash
    Preconditions: Blender installed and on PATH (no external .blend file required)
    Steps:
      1. Run `blender --factory-startup --background --python blender/export_camera_data.py -- --output build/test_camera_export.json --start 1 --end 5`
         (--factory-startup creates a default scene with a camera at a known position)
      2. Verify build/test_camera_export.json exists and is valid JSON (parse with Python: `python -c "import json; json.load(open('build/test_camera_export.json'))"`)
      3. Verify "frames" key exists with entries "0001" through "0005"
      4. Verify each frame has: matrix_world (4x4 array), projection (4x4 array), fov (float > 0), aspect_ratio (float > 0), near_clip (float > 0), far_clip (float > near_clip)
    Expected Result: Valid camera.json with 5 frame entries, all fields populated with non-zero values
    Failure Indicators: Script error, invalid JSON, missing fields, zero FOV
    Evidence: .sisyphus/evidence/task-6-camera-export.txt

  Scenario: Script errors gracefully when no active camera
    Tool: Bash
    Preconditions: Blender installed
    Steps:
      1. Create a minimal Python wrapper that removes all cameras before running export:
         `blender --factory-startup --background --python-expr "import bpy; [bpy.data.objects.remove(o) for o in bpy.data.objects if o.type=='CAMERA']; bpy.context.scene.camera=None; exec(open('blender/export_camera_data.py').read())"`
         Or alternatively: pipe a script that deletes cameras then calls export
      2. Check exit code is non-zero
      3. Check stderr/stdout contains "camera" error message
    Expected Result: Clean error exit with descriptive message about missing camera
    Failure Indicators: Exit 0, crash, Python traceback without useful message
    Evidence: .sisyphus/evidence/task-6-no-camera-error.txt
  ```

  > **NOTE**: Task 6 QA uses `--factory-startup` which provides Blender's default scene (with a camera, light, and cube) — no external `.blend` fixtures needed.

  **Commit**: YES
  - Message: `feat(blender): add export_camera_data.py for camera matrix export`
  - Files: `blender/export_camera_data.py`
  - Pre-commit: `python -c "import ast; ast.parse(open('blender/export_camera_data.py').read())"`

---

- [x] 7. SequenceProcessor Frame-Pair Interpolation Loop

  **What to do**:
  - Extend `SequenceProcessor::processDirectory()` (or add new method `processDirectoryWithInterpolation()`) to:
    1. **Check config**: If `config.interpolateFactor > 0`, enter FG processing mode
    2. **Load camera data**: Create `CameraDataLoader`, call `load(config.cameraDataFile, errorMsg)`. Error if load fails.
    3. **Create DLSS-G feature**: Call `ngx.createDlssFG(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, cmdBuf, errorMsg)`. Error if creation fails (with hardware check message).
    4. **Determine multiFrameCount**: For 2x → `multiFrameCount=1`. For 4x → `multiFrameCount=3`. Validate against `ngx.maxMultiFrameCount()` — if 4x requested but max < 3, error: "4x frame generation requires RTX 50 series or newer"
    5. **Frame-pair processing loop**:
       - Iterate through sorted EXR files: for each pair (frame N, frame N+1):
         a. Load frame N EXR (color as backbuffer, depth, motion vectors) — upload to GPU textures
         b. If frame N is first frame or there's a gap from previous, set `reset=true`
         c. Compute camera params: `cameraLoader.computePairParams(frameNumN, frameNumN+1)`
         d. For each `multiFrameIndex` from 1 to `multiFrameCount`:
            - Allocate output texture for interpolated frame
            - Fill `DlssFgFrameInput` with current frame textures, camera params, reset flag, multiFrameCount, multiFrameIndex
            - Call `fgProcessor.evaluate(cmdBuf, input, errorMsg)`
            - Download output texture to CPU buffer
            - Write output EXR with interpolated frame naming
         e. Write original frame N as output EXR (re-numbered)
         f. Keep frame N's textures as "previous frame" for next iteration (don't free yet)
         g. Free frame N-1's textures (the one before previous)
       - After last pair: write final frame (frame N+1) as output EXR
    6. **Output naming convention**: Re-number entire sequence:
       - For 2x: original_0001→out_0001, interp→out_0002, original_0002→out_0003, interp→out_0004, ...
       - For 4x: original_0001→out_0001, interp1→out_0002, interp2→out_0003, interp3→out_0004, original_0002→out_0005, ...
       - Zero-pad output frame numbers to at least 4 digits (or match input padding)
    7. **Cleanup**: Release DLSS-G feature via `ngx.releaseDlssFG()`, free all GPU textures
  - Add `DlssFgFeatureGuard` (RAII) similar to existing `DlssFeatureGuard` for cleanup safety
  - Handle edge cases:
    - Single input frame: Error "At least 2 frames required for interpolation"
    - Frame gap detection: Use existing `extractTrailingFrameNumber()` — if gap > 1 between consecutive files, set `reset=true`
    - Camera data missing for a frame: Error with specific frame number

  **Must NOT do**:
  - Do NOT modify existing DLSS-RR processing path — keep it separate
  - Do NOT add pipeline mode (RR → FG chaining)
  - Do NOT add GUI/viewer support
  - Do NOT modify output EXR format (keep R16G16B16A16_SFLOAT half-float RGBA)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Complex processing loop with temporal state management, frame-pair buffering, output naming, and edge case handling
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (after Task 5 completes)
  - **Blocks**: Tasks 8, 9
  - **Blocked By**: Tasks 3 (CameraDataLoader), 4 (CLI flags), 5 (DlssFgProcessor)

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:1-80` — File structure, includes, helper functions. Follow same patterns.
  - `src/pipeline/sequence_processor.cpp:28-36` — `DlssFeatureGuard` RAII pattern. Create `DlssFgFeatureGuard` following same pattern.
  - `src/pipeline/sequence_processor.cpp:46-61` — `extractTrailingFrameNumber()` — reuse for frame number extraction and gap detection.
  - `src/pipeline/sequence_processor.cpp:80-576` — `processDirectory()` method — the main processing loop. Study it carefully for: frame iteration, texture upload/download, command buffer management, EXR write, cleanup. The FG loop should follow the same structure but with frame-pair semantics.

  **API/Type References**:
  - `src/dlss/dlss_fg_processor.h` (Task 5) — `DlssFgFrameInput` struct and `DlssFgProcessor::evaluate()` signature
  - `src/core/camera_data_loader.h` (Task 3) — `CameraDataLoader::load()`, `computePairParams()`, `hasFrame()`
  - `src/cli/config.h` (Task 4) — `AppConfig::interpolateFactor` and `AppConfig::cameraDataFile`

  **WHY Each Reference Matters**:
  - `sequence_processor.cpp:80-576`: This is the PRIMARY reference. The FG loop is structurally similar but processes pairs instead of individual frames. Study the texture lifecycle (upload → process → download → write → free), command buffer fencing, and error handling.
  - `extractTrailingFrameNumber()`: Reuse this for both gap detection AND output file naming. Existing code handles the input side; FG adds output re-numbering.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: 2-frame sequence produces 3 output EXRs with 2x interpolation
    Tool: Bash
    Preconditions: tests/fixtures/sequence/ has at least 2 EXR frames, camera.json exists
    Steps:
      1. Run `dlss-compositor.exe --input-dir tests/fixtures/sequence --output-dir test_fg_out --interpolate 2x --camera-data tests/fixtures/camera.json`
      2. Count output EXR files in test_fg_out/
      3. Verify count = 3 (original_1 + interp + original_2)
      4. Verify files named sequentially (0001, 0002, 0003)
    Expected Result: 3 output EXR files with sequential naming
    Failure Indicators: Wrong count, wrong naming, crash, error message
    Evidence: .sisyphus/evidence/task-7-2x-output.txt

  Scenario: Single input frame produces error
    Tool: Bash
    Preconditions: Directory with only 1 EXR file
    Steps:
      1. Run `dlss-compositor.exe --input-dir single_frame_dir --output-dir out --interpolate 2x --camera-data camera.json`
      2. Check exit code is non-zero
      3. Check stderr mentions "at least 2 frames"
    Expected Result: Non-zero exit, helpful error about minimum frame count
    Failure Indicators: Exit 0, crash, generic error
    Evidence: .sisyphus/evidence/task-7-single-frame-error.txt

  Scenario: HDR preservation — output values > 1.0
    Tool: Bash (Python validator)
    Preconditions: Input EXRs contain pixel values > 1.0 (HDR), interpolation completed
    Steps:
      1. Run interpolation on HDR test sequence
      2. Use Python/OpenEXR to read interpolated output EXR
      3. Check max pixel value > 1.0 (not clamped to SDR range)
    Expected Result: Output contains HDR values, no clamping
    Failure Indicators: Max value ≤ 1.0, all-zero pixels
    Evidence: .sisyphus/evidence/task-7-hdr-preservation.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): add frame-pair interpolation loop to SequenceProcessor`
  - Files: `src/pipeline/sequence_processor.cpp`, `src/pipeline/sequence_processor.h`
  - Pre-commit: `cmake --build build --config Release`

---

- [x] 8. Catch2 Test Suite — Camera Loader, CLI, and FG Processor Tests

  **What to do**:
  - Create `tests/test_camera_data_loader.cpp`:
    - `TEST_CASE("camera_data_loader_parse_valid", "[camera]")` — Load valid camera.json fixture, verify frame count, FOV, matrix_world populated
    - `TEST_CASE("camera_data_loader_parse_invalid", "[camera]")` — Malformed JSON returns false with error message
    - `TEST_CASE("camera_data_loader_missing_frame", "[camera]")` — Request non-existent frame, verify error
    - `TEST_CASE("camera_data_compute_pair_params", "[camera]")` — Two-frame pair, verify clipToPrevClip is non-identity, clipToLensClip is identity
  - Create `tests/test_dlss_fg_processor.cpp`:
    - `TEST_CASE("dlss_fg_availability", "[fg]")` — Call isDlssFGAvailable(), SKIP if false (non-RTX-40 hardware), verify true on RTX 40+
    - `TEST_CASE("dlss_fg_feature_creation", "[fg]")` — Create DLSS-G feature headless, verify handle is non-null, SKIP if not available
    - `TEST_CASE("dlss_fg_single_evaluation", "[fg]")` — Full evaluate with test textures, verify non-zero output, SKIP if not available
  - Extend `tests/test_cli.cpp`:
    - Add test cases for `--interpolate 2x`, `--interpolate 4x`, invalid value, missing `--camera-data`
  - Create `tests/fixtures/camera.json` — Test fixture with 5 frame entries matching existing test EXR sequence
  - Update `tests/CMakeLists.txt`:
    - Add `test_camera_data_loader.cpp` and `test_dlss_fg_processor.cpp` to test executable sources
    - Add CameraDataLoader source to test link if needed

  **Must NOT do**:
  - Do NOT modify existing test files beyond adding new test cases to test_cli.cpp
  - Do NOT require RTX 40+ for test suite to pass (use SKIP for hardware-dependent tests)
  - Do NOT add visual quality regression tests

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Multiple test files, fixture creation, and integration with existing Catch2 infrastructure
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 9)
  - **Parallel Group**: Wave 4 (with Task 9)
  - **Blocks**: Task 9
  - **Blocked By**: Tasks 2, 3, 4, 5, 7 (needs all implementation complete)

  **References**:

  **Pattern References**:
  - `tests/test_dlss_rr_processor.cpp:1-50` — Test file structure, includes, SKIP macro pattern for hardware-dependent tests. Follow the exact `SKIP("reason")` pattern for non-RTX-40 hardware.
  - `tests/test_dlss_rr_processor.cpp:137-236` — GPU integration test structure: VulkanContext init, TextureGuard, command buffer allocation, submit/wait pattern. Replicate for FG tests.
  - `tests/test_cli.cpp` — Existing CLI tests. Add new TEST_CASEs following the same assertion pattern.
  - `tests/test_scaffold.cpp` — Catch2 main entry point. Verify new test files are included in CMakeLists.txt.

  **API/Type References**:
  - `src/core/camera_data_loader.h` (Task 3) — Class interface being tested
  - `src/dlss/dlss_fg_processor.h` (Task 5) — Class interface being tested
  - `src/cli/config.h` (Task 4) — AppConfig fields being tested

  **WHY Each Reference Matters**:
  - `test_dlss_rr_processor.cpp:137-236`: The GPU test pattern is complex — VulkanContext lifecycle, texture allocation/deallocation, command buffer management. Don't reinvent; copy the proven pattern.
  - `test_cli.cpp`: Ensures new CLI tests follow the same assertion style and tag conventions.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: All camera loader tests pass
    Tool: Bash
    Preconditions: Build succeeds with test_camera_data_loader.cpp
    Steps:
      1. Run `dlss-compositor-tests "[camera]"`
      2. Verify all tests pass (0 failures)
    Expected Result: All camera tests pass
    Failure Indicators: Test failures, build errors
    Evidence: .sisyphus/evidence/task-8-camera-tests.txt

  Scenario: All CLI tests pass (including new interpolate tests)
    Tool: Bash
    Preconditions: Build succeeds
    Steps:
      1. Run `dlss-compositor-tests "[cli]"`
      2. Verify all tests pass including new --interpolate tests
    Expected Result: All CLI tests pass
    Failure Indicators: Test failures
    Evidence: .sisyphus/evidence/task-8-cli-tests.txt

  Scenario: FG tests SKIP gracefully on non-RTX-40 hardware
    Tool: Bash
    Preconditions: Build succeeds, any GPU (including non-RTX-40)
    Steps:
      1. Run `dlss-compositor-tests "[fg]"`
      2. If RTX 40+: verify tests pass
      3. If not RTX 40+: verify tests are SKIPPED (not FAILED)
    Expected Result: Tests pass on RTX 40+ or skip gracefully on other hardware
    Failure Indicators: Tests FAIL (not skip) on non-RTX-40 hardware
    Evidence: .sisyphus/evidence/task-8-fg-tests.txt
  ```

  **Commit**: YES
  - Message: `test: add Catch2 tests for DLSS-FG camera loader, CLI, and processor`
  - Files: `tests/test_camera_data_loader.cpp`, `tests/test_dlss_fg_processor.cpp`, `tests/test_cli.cpp`, `tests/CMakeLists.txt`, `tests/fixtures/camera.json`
  - Pre-commit: `dlss-compositor-tests`

---

- [x] 9. Integration Test — End-to-End 2-Frame Interpolation

  **What to do**:
  - Add integration test to `tests/test_integration.cpp` (or new `tests/test_fg_integration.cpp`):
    - `TEST_CASE("dlss_fg_e2e_interpolation", "[integration][fg]")`:
      1. SKIP if `!ngx.isDlssFGAvailable()`
      2. Set up: Use existing test fixture EXRs (`tests/fixtures/sequence/frame_0001.exr`, `frame_0002.exr`)
      3. Create camera.json fixture if not already present (from Task 8)
      4. Run the full SequenceProcessor with `config.interpolateFactor = 2` and `config.cameraDataFile = "tests/fixtures/camera.json"`
      5. Verify output directory contains 3 EXR files (2 originals + 1 interpolated)
      6. Read interpolated EXR and verify:
         - Non-zero pixel data (not all black)
         - Resolution matches input resolution
         - Format is half-float (R16G16B16A16_SFLOAT)
         - At least some pixels have values > 1.0 (HDR preserved, if input has HDR values)
  - This is the ultimate validation that the entire pipeline works end-to-end

  **Must NOT do**:
  - Do NOT test 4x separately (RTX 50 may not be available; 4x SKIP is enough)
  - Do NOT add visual quality assertions (subjective)
  - Do NOT modify existing integration tests

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: End-to-end integration requiring all components working together, complex setup/teardown
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (with Task 8)
  - **Parallel Group**: Wave 4 (with Task 8)
  - **Blocks**: FINAL verification wave
  - **Blocked By**: All implementation tasks (1-7)

  **References**:

  **Pattern References**:
  - `tests/test_integration.cpp` — Existing integration test structure. Add new TEST_CASE following the same pattern.
  - `tests/test_sequence_processor.cpp` — Sequence processor test patterns for fixture loading and output validation.

  **API/Type References**:
  - `src/pipeline/sequence_processor.h` — `processDirectory()` or new FG processing method
  - `src/core/exr_reader.h` — For validating output EXR content
  - `tests/fixtures/sequence/` — Existing 5-frame test sequence

  **WHY Each Reference Matters**:
  - `test_integration.cpp`: The existing integration test establishes the full lifecycle pattern — init VulkanContext, init NgxContext, create features, process, verify output, cleanup. Follow the same structure.
  - `tests/fixtures/sequence/`: These EXR files are the actual test inputs. Verify they contain depth and motion vector channels needed for DLSS-G.

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: End-to-end 2x interpolation produces valid output
    Tool: Bash
    Preconditions: All implementation tasks complete, RTX 40+ GPU
    Steps:
      1. Run `dlss-compositor-tests "[integration][fg]"`
      2. Verify test passes (or SKIPs on non-RTX-40)
      3. If passed: verify output directory has 3 EXR files
      4. Verify each output EXR opens without error
    Expected Result: Integration test passes, 3 valid output EXRs
    Failure Indicators: Test failure, wrong file count, corrupt EXRs
    Evidence: .sisyphus/evidence/task-9-e2e-integration.txt

  Scenario: Integration test SKIPs gracefully without RTX 40+
    Tool: Bash
    Preconditions: Non-RTX-40 GPU
    Steps:
      1. Run `dlss-compositor-tests "[integration][fg]"`
      2. Verify test is SKIPPED (not FAILED)
      3. Verify exit code is 0
    Expected Result: Clean SKIP with descriptive message
    Failure Indicators: FAIL instead of SKIP, crash
    Evidence: .sisyphus/evidence/task-9-skip-graceful.txt
  ```

  **Commit**: YES
  - Message: `test: add end-to-end DLSS-FG integration test`
  - Files: `tests/test_integration.cpp` (or `tests/test_fg_integration.cpp`), `tests/fixtures/camera.json`
  - Pre-commit: `dlss-compositor-tests "[integration]"`

---

## Final Verification Wave

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.

- [ ] F1. **Plan Compliance Audit** — `oracle`
  Read this plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns (Streamline imports, RIFE references, tonemap calls) — reject with file:line if found. Check evidence files exist in `.sisyphus/evidence/`. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [ ] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build build --config Release 2>&1` to check warnings. Review all changed/new files for: `as any`/`@ts-ignore` (N/A for C++), empty catches, `printf`/`std::cout` in prod code (should use logging), commented-out code, unused includes. Check AI slop: excessive comments, over-abstraction, generic variable names (data/result/item/temp). Verify consistent coding style with existing codebase.
  Output: `Build [PASS/FAIL] | Warnings [N] | Files [N clean/N issues] | VERDICT`

- [ ] F3. **Real Manual QA** — `unspecified-high`
  Start from clean build. Execute EVERY QA scenario from EVERY task — follow exact steps, capture evidence. Test with actual 2-frame EXR fixture sequence + camera.json. Verify output EXR files exist with correct names and non-zero content. Test error cases: missing camera.json, invalid --interpolate value, non-RTX-40 GPU fallback. Save to `.sisyphus/evidence/final-qa/`.
  Output: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

- [ ] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (`git log/diff`). Verify 1:1 — everything in spec was built (no missing), nothing beyond spec was built (no creep). Check "Must NOT do" compliance. Detect cross-task contamination: Task N touching Task M's files. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

| Commit | Scope | Message | Files | Pre-commit |
|--------|-------|---------|-------|-----------|
| 1 | CMake DLL copy | `build: add nvngx_dlssg.dll copy to build output` | CMakeLists.txt, tests/CMakeLists.txt | `cmake --build build --config Release` |
| 2 | Camera loader | `feat(core): add CameraDataLoader for per-frame camera JSON parsing` | src/core/camera_data_loader.h/.cpp | build succeeds |
| 3 | NgxContext FG | `feat(dlss): add DLSS-G feature lifecycle to NgxContext` | src/dlss/ngx_wrapper.h/.cpp | build succeeds |
| 4 | CLI flags | `feat(cli): add --interpolate and --camera-data flags` | src/cli/config.h, src/cli/cli_parser.cpp | build succeeds |
| 5 | FG Processor | `feat(dlss): add DlssFgProcessor for DLSS Frame Generation evaluation` | src/dlss/dlss_fg_processor.h/.cpp | build succeeds |
| 6 | Sequence loop | `feat(pipeline): add frame-pair interpolation loop to SequenceProcessor` | src/pipeline/sequence_processor.cpp/.h | build succeeds |
| 7 | Blender script | `feat(blender): add export_camera_data.py for camera matrix export` | blender/export_camera_data.py | Python syntax check |
| 8 | Tests | `test: add Catch2 tests for DLSS-FG camera loader, CLI, and processor` | tests/test_camera_data_loader.cpp, tests/test_dlss_fg_processor.cpp, extended tests/test_cli.cpp, tests/CMakeLists.txt | `dlss-compositor-tests` |
| 9 | Integration | `test: add end-to-end DLSS-FG integration test` | tests/test_integration.cpp, tests/fixtures/camera.json | `dlss-compositor-tests "[integration]"` |

---

## Success Criteria

### Verification Commands
```bash
cmake --build build --config Release  # Expected: BUILD SUCCEEDED, 0 warnings
build\Release\dlss-compositor-tests.exe  # Expected: All tests passed
build\Release\dlss-compositor.exe --input-dir tests\fixtures\sequence --output-dir test_fg_out --interpolate 2x --camera-data tests\fixtures\camera.json  # Expected: interpolated EXR files in test_fg_out
```

### Final Checklist
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
- [ ] All Catch2 tests pass
- [ ] Build succeeds with no new warnings
- [ ] nvngx_dlssg.dll copied to build output alongside nvngx_dlssd.dll
- [ ] Interpolated EXR files contain non-zero HDR data (values > 1.0 preserved)
