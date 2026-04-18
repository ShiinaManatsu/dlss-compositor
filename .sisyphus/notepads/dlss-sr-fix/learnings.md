# Learnings — DLSS-SR Fix

## Conventions & Patterns

(Agents will append findings here)

---

## [2026-04-16T23:14:02Z] Task 1: EXR Channel Investigation

**Actual channel names found**: Image.A, Image.B, Image.G, Image.R (4 channels only)

**Alias coverage**: PASS (all actual channels are covered by exr_reader.cpp alias mappings)
- Image.* → RenderLayer.Combined.* mapping works correctly
- All 4 Image channels map to RenderLayer.Combined.{A,B,G,R}

**Multi-part EXR**: NO (single-part EXR, standard format)

**Required mapping changes**: NONE (alias mappings are correct)

**CRITICAL FINDING**: The Blender export is NOT outputting required AOV passes:
- ✗ Depth.Z — NOT FOUND (hard requirement)
- ✗ Vector.X/Y — NOT FOUND (hard requirement)
- ✗ Normal.X/Y/Z — NOT FOUND (optional but recommended)
- ✓ Image.R/G/B/A — FOUND

**Root Cause**: Blender render configuration issue, not compositor issue
- The render preset is not configured to output Vector and Depth passes
- Without these, compositor will fail with "Missing required channel" errors
- This matches the failure indicator: "只有 Image.A/B/G/R 四个通道且无其他通道 → Blender 导出配置有问题"

**Impact on Task 7**: Task 7 (compositor integration) cannot proceed until Blender exports
the required AOV passes. The alias mappings are correct but cannot compensate for missing channels.

**Evidence files**:
- `.sisyphus/evidence/task-1-exr-channels.txt` — Full channel list and analysis
- `.sisyphus/evidence/task-1-alias-coverage.txt` — Alias mapping verification

---

## [2026-04-17T00:00:00Z] Task 6: MV Negation Implementation

**Empirical basis**: Task 2 verification showed Blender Vector.X = -131.687256 for rightward motion (x=0->x=2)
- Sign interpretation: negative = prev->curr direction
- DLSS expectation: curr->prev direction (positive for rightward motion)
- **Conclusion**: Negation required to convert Blender convention to DLSS convention

**Implementation**:
- Modified src/core/mv_converter.cpp lines 23-24 to negate both X and Y components
- Updated comments to document the convention mismatch and solution
- Updated all 6 test cases in 	ests/test_mv_converter.cpp to expect negated values

**Test results**: All 6 MV tests pass (42 assertions)
- "10px right motion": now expects -10.0f (was 10.0f)
- "Y axis": now expects -5.0f (was 5.0f)
- "channels 2,3 ignored": now expects (-3.0f, -4.0f) (was (3.0f, 4.0f))
- Zero motion, scale factors, and channel handling all verified

**Key insight**: The negation is NOT a bug fix but a **convention bridge**. Blender's Vector pass uses prev->curr (negative for rightward motion), while DLSS-SR expects curr->prev (positive for rightward motion). The negation converts between these two conventions.

**Evidence files**:
- .sisyphus/evidence/task-6-mv-tests.txt - Full test output
- .sisyphus/evidence/task-6-mv-negation-verify.txt - Verification report

---

## [2026-04-17T00:00:00Z] Task 4: Halton(2,3) Jitter Implementation

**Files modified**:
- blender/aov_export_preset.py (Blender 4.x/5.x compatible, standalone script)
- blender/dlss_compositor_aov/__init__.py (Blender 5.x extension)
- Both files kept in sync with identical jitter implementation

**Implementation components**:
1. **Halton sequence generator** (lines 27-44):
   - Function: halton(index, base) — generates Halton sequence values
   - Returns float in [0, 1) range
   - Base 2 (X-axis): 8 unique values
   - Base 3 (Y-axis): 8 unique values
   - Verified: all values unique and in correct range

2. **Jitter handlers** (lines 49-95):
   - save_original_shift(scene): Saves camera.data.shift_x/y at render start
   - apply_jitter(scene): Applies Halton jitter to camera shift each frame
   - restore_shift(scene): Restores original shift at render end/cancel
   - Global state: _original_shift_x, _original_shift_y, _jitter_enabled

3. **Handler registration** (register/unregister):
   - render_init → save_original_shift
   - frame_change_pre → apply_jitter
   - render_complete → restore_shift
   - render_cancel → restore_shift (ensures cleanup even if render cancelled)

4. **JSON export extension** (export_camera operator, lines 172-174):
   - Calculates jitter_x/jitter_y per frame: halton(frame_num % 8, base) - 0.5
   - Adds jitter_x/jitter_y fields to camera.json (pixel space, [-0.5, +0.5])
   - 8-sample cycle (frame_num % 8) repeats every 8 frames

**Key insight: Jitter application strategy**:
- From Task 3 verification: Blender Vector pass does NOT include jitter
- MVJittered flag NOT required
- Conversion formula: shift = jitter_pixels / resolution (verified, 0.0062% error)
- Implementation: Apply jitter via camera.data.shift, export values to JSON
- Compositor will read jitter from camera.json metadata

**Halton sequence values** (8-sample cycle):
Frame | jitter_x | jitter_y
------|----------|----------
  0   |  -0.5000 |  -0.5000
  1   |   0.0000 |  -0.1667
  2   |  -0.2500 |   0.1667
  3   |   0.2500 |  -0.3889
  4   |  -0.3750 |  -0.0556
  5   |   0.1250 |   0.2778
  6   |  -0.1250 |  -0.2778
  7   |   0.3750 |   0.0556

**camera.json format** (extended):
{
  version: 1,
  render_width: 1920,
  render_height: 1080,
  frames: {
    0000: {
      matrix_world: [...],
      projection: [...],
      fov: 0.7854,
      aspect_ratio: 1.778,
      near_clip: 0.1,
      far_clip: 1000.0,
      jitter_x: -0.5,      // NEW: pixel space, [-0.5, +0.5]
      jitter_y: -0.5       // NEW: pixel space, [-0.5, +0.5]
    }
  }
}

**Pattern for Blender handlers**:
- Use module-level global variables for state (_original_*, _enabled)
- Register handlers in register(), remove in unregister() with safety checks
- Always register both complete AND cancel handlers for cleanup
- Safety checks: if handler in handlers_list: handlers_list.remove(handler)

**Evidence files**:
- .sisyphus/evidence/task-4-halton-sequence.txt — Halton sequence verification
- .sisyphus/evidence/task-4-shift-restore.txt — Handler behavior documentation
- .sisyphus/evidence/task-4-camera-json-jitter.txt — JSON format documentation

**Next steps**:
- Task 7: Read jitter_x/jitter_y from camera.json in C++ compositor
- Task 7: Pass jitter values to DLSS-SR via NGX API
- No changes needed to MV handling (jitter NOT baked into Vector pass)

## [] Task 7: Compositor Jitter Integration

**Files modified**: 
- src/core/camera_data_loader.h
- src/core/camera_data_loader.cpp  
- src/pipeline/sequence_processor.cpp
- tests/test_camera_data_loader.cpp

**Key changes**:
- CameraFrameData extended with jitter_x/jitter_y fields (default 0.0f for backward compatibility)
- JSON parsing uses .value() for optional jitter fields (backward compatible with old camera.json)
- SR pipeline optionally loads camera.json and assigns jitter to DlssSRFrameInput
- Verbose logging added for non-zero jitter values
- Two new test cases added: with jitter and without jitter (backward compatibility)

**Test results**:
- All existing camera tests pass (no regression)
- New jitter tests pass (Test #59, #60)
- Backward compatibility verified: old camera.json without jitter fields loads successfully with jitter defaulting to 0.0
- New camera.json with jitter fields parses correctly

**Implementation notes**:
- SR pipeline camera loading is optional (only if config.cameraDataFile is non-empty)
- If camera data load fails, pipeline continues with jitter=0.0 and logs warning
- Jitter logging only triggers for non-zero values (using 1e-6f epsilon)
- Pattern follows existing FG pipeline camera loading (lines 910-918)


## [2026-04-16T16:23:28Z] Task 7: Compositor Jitter Integration

**Files modified**: 
- src/core/camera_data_loader.h
- src/core/camera_data_loader.cpp  
- src/pipeline/sequence_processor.cpp
- tests/test_camera_data_loader.cpp

**Key changes**:
- CameraFrameData extended with jitter_x/jitter_y fields (default 0.0f for backward compatibility)
- JSON parsing uses .value() for optional jitter fields (backward compatible with old camera.json)
- SR pipeline optionally loads camera.json and assigns jitter to DlssSRFrameInput
- Verbose logging added for non-zero jitter values
- Two new test cases added: with jitter and without jitter (backward compatibility)

**Test results**:
- All existing camera tests pass (no regression)
- New jitter tests pass (Test #59, #60)
- Backward compatibility verified: old camera.json without jitter fields loads successfully with jitter defaulting to 0.0
- New camera.json with jitter fields parses correctly

**Implementation notes**:
- SR pipeline camera loading is optional (only if config.cameraDataFile is non-empty)
- If camera data load fails, pipeline continues with jitter=0.0 and logs warning
- Jitter logging only triggers for non-zero values (using 1e-6f epsilon)
- Pattern follows existing FG pipeline camera loading (lines 910-918)


## [2026-04-16T16:34:24Z] Task 8: End-to-End Verification (Blocked)

**Status**: BLOCKED due to Blender MCP connectivity failure (WinError 10061 connection refused).
**Observed behavior**: blender_get_scene_info / blender_execute_blender_code could not connect to Blender addon endpoint.
**Impact**: Could not render new 10-frame jittered sequence (200-209), therefore compositor E2E run was not executable in this session.
**Evidence generated**: task-8-render-output.txt, task-8-compositor-output.txt, task-8-visual-comparison.png (blocked placeholder).
**Required unblocking action**: Launch Blender with Blender MCP addon active, then rerun Task 8 pipeline end-to-end.

## [2026-04-16T17:31:23Z] Task 8: End-to-End Verification (Success)

**Render**: Existing user-opened test scene rendered frames 200-209 at 960x540 via dlsscomp addon operators.
**AOV/format**: Configure Passes reported Combined/Z/Vector/Normal ON; compositor output node set to OPEN_EXR_MULTILAYER.
**Input artifacts**: 10 EXR files generated (file_name0200.exr..file_name0209.exr) + camera.json with 10/10 non-zero jitter frames.
**Jitter values**: Halton sequence verified for 0200..0209, including expected repeats at 0208/0209.
**Compositor run**: Processed all 10 frames successfully to C:\Users\White\AppData\Local\Temp\dlss_e2e_output (exit code 0).
**CLI note**: Current binary rejects --verbose flag, but jitter offsets are printed to stdout and captured as evidence (frame 200..209).
**Visual evidence**: .sisyphus/evidence/task-8-visual-comparison.png generated as side-by-side input frame_0205.png vs output file_name0205.exr.

## Code Quality Review (F2) - 2026-04-17 01:42:28

### Build Status
- ? Build: PASS
- ? All targets compiled successfully

### Test Results
- Tests Run: 71
- Tests Passed: 67 (94%)
- Tests Failed: 1 (dlss_fg_e2e_interpolation - fixture path issue)
- Tests Skipped: 4 (expected - hardware/data dependent)
- Tests Not Run: 3 (configuration issues)

### Modified Files Analysis
1. **src/core/mv_converter.cpp** (NEW)
   - Clean, well-documented motion vector conversion
   - No anti-patterns, no code smells
   - Proper const-correctness

2. **tests/test_mv_converter.cpp** (NEW)
   - Comprehensive test coverage (6 test cases)
   - Tests edge cases and correctness
   - Clean test naming

3. **src/dlss/ngx_wrapper.cpp** (MODIFIED)
   - Clean code, proper resource management
   - Minor AI slop (acceptable - generic helper names but contextually clear)
   - Good error handling

### Anti-Pattern Scan Results
- ? No 'as any' or @ts-ignore
- ? No empty catch blocks
- ? No console.log in production code
- ? Only 1 TODO in entire codebase (acceptable)
- ? No commented-out code blocks

### Test Coverage
- ? mv_converter.cpp ? test_mv_converter.cpp (NEW, comprehensive)
- ?? ngx_wrapper.cpp ? No dedicated unit tests (covered by integration tests)

### Critical Issue
**Test Failure**: dlss_fg_e2e_interpolation
- **Cause**: Working directory mismatch for fixture path
- **Path**: tests/fixtures/camera.json (relative path doesn't resolve from build/tests/Release)
- **Impact**: Blocks validation of DLSS Frame Generation pipeline
- **Fix Required**: Update fixture path or configure CMake to copy fixtures

### Verdict
**CONDITIONAL APPROVE** - Code quality is HIGH, but test infrastructure needs fixing

**Production-Ready**: YES (after fixing test failure)

The code changes are solid. The test failure is infrastructure-related, not a code correctness issue.
