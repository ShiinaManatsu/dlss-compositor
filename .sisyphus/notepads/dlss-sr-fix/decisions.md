# Architectural Decisions — DLSS-SR Fix

## Key Decisions

(Agents will append architectural choices here)

---

## [2026-04-16T23:25:30Z] Task 3: Jitter-MV Interaction (Blocked by Blender MCP connectivity)

**Baseline (shift=0)**: Not measured (Blender MCP unavailable; no render output)

**With shift_x=0.001**: Not measured (Blender MCP unavailable; no render output)

**Jitter baked into MV**: UNDETERMINED (insufficient runtime evidence)

**Conversion formula**: `jitter_pixels = shift_x * resolution_x` remains **theoretical only** in this run (empirical verification blocked)

**Implication for Task 4**: Cannot finalize `MVJittered` strategy until Task 3 is rerun with working Blender MCP session. Keep implementation path flexible for both outcomes.

**Blocking detail**:
- `blender_execute_blender_code` and `blender_get_scene_info` both returned connection errors.
- `blender-mcp.exe` reported WinError 10061 (connection refused), indicating Blender addon endpoint is not running/reachable.

---

## [2026-04-16T23:38:30Z] Task 3: Jitter-MV Interaction

**Baseline (shift=0)**: Vector.X/Y = 0 across frame 201 (`abs_max=0.0`, `abs_mean=0.0`)

**With shift_x=0.001**: Vector.X/Y still 0 across frame 201; delta vs baseline is 0 everywhere

**Jitter baked into MV**: **NO**

**Conversion formula**: `jitter_pixels = shift_x * resolution_x` **VERIFIED**
- At 640px width and shift_x=0.001, theoretical = 0.64px
- Projection-space measurement magnitude = 0.6400396px (error 0.0062%)

**Implication for Task 4**: `MVJittered` flag **NOT REQUIRED** for Blender Vector pass path in this static jitter test. Jitter should be conveyed/handled outside Blender MV (e.g., camera/jitter metadata path).

## [2026-04-16T15:50:20Z] Task 2: MV Direction Verification

**Test setup**: Cube moving from x=0 to x=2 (rightward, +X direction), frame 200->201, 256x256, Vector pass enabled, output EXR: C:/Users/White/AppData/Local/Temp/dlss_mv_test/mv_test_ml_00201.exr
**Vector.X value in moving region**: center patch mean = -131.687256, object mean = -128.141754 (negative)
**Sign interpretation**: negative = prev→curr
**DLSS expectation**: curr→prev (positive for rightward motion)
**Conclusion**: Negation REQUIRED for Task 6
