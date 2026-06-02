# Unresolved Blockers — DLSS-SR Fix

## Active Blockers

---

## [2026-04-16T23:30:00Z] CRITICAL: Wave 0 Revealed Fundamental Blockers

### Blocker 1: Blender MCP Unavailable (HIGH PRIORITY)

**Impact**: Tasks 2, 3, and 8 cannot execute
**Status**: BLOCKED — external dependency

**Details**:
- Tasks 2 and 3 require Blender MCP to render test frames for MV direction and jitter-MV interaction verification
- All Blender MCP tool calls return: "Could not connect to Blender. Make sure the Blender addon is running."
- `blender` executable not found in PATH
- `blender-mcp.exe` exists but reports WinError 10061 (connection refused)

**Required Action**:
1. Start Blender application
2. Ensure Blender MCP addon is installed and active
3. Verify addon endpoint is reachable (default: localhost:3000 or configured port)

**Affected Tasks**:
- Task 2: MV direction verification (session: `ses_269231180ffeXq5MB0A6Fu0hqd`)
- Task 3: Jitter-MV interaction verification (session: `ses_269226f23ffeJbq01nk3j023PW`)
- Task 8: End-to-end verification rendering

**Workaround Options**:
- Option A: Skip Tasks 2-3, proceed with Tasks 4-7 based on theoretical assumptions (risky — may implement wrong MV direction)
- Option B: Pause plan execution until Blender MCP is available (recommended)

---

### Blocker 2: Missing AOV Passes in Existing Renders (CRITICAL — ROOT CAUSE)

**Impact**: This is the ROOT CAUSE of the DLSS quality issues
**Status**: CONFIRMED — requires Blender re-render

**Details**:
- Task 1 investigation revealed that `E:\Render Output\Compositing\SnowMix\sequences\4k_aov\file_name_0200.exr` contains ONLY 4 channels:
  - Image.R, Image.G, Image.B, Image.A
- **MISSING required channels**:
  - ✗ Depth.Z (hard requirement for DLSS)
  - ✗ Vector.X/Y (hard requirement for DLSS temporal accumulation)
  - ✗ Normal.X/Y/Z (optional but recommended)
- Without these channels, DLSS receives zero/default values for motion vectors and depth
- **This explains why DLSS-SR Preset L has no visible effect** — temporal accumulation cannot work without motion vectors

**Root Cause**:
- Blender render configuration is not exporting AOV passes
- The `blender/aov_export_preset.py` or `blender/dlss_compositor_aov/__init__.py` "Configure Passes" operation was either:
  - Not executed before rendering
  - Executed but failed to configure passes correctly
  - Executed but user rendered with wrong output settings

**Required Action**:
1. Open Blender project
2. Run "Configure Passes" operation from DLSS Compositor panel
3. Verify in Render Properties > Passes that Vector and Depth passes are enabled
4. Re-render the sequence (at least 10 frames for testing)
5. Verify new EXR files contain Vector.X/Y and Depth.Z channels

**Affected Tasks**:
- Task 7: Compositor integration (cannot test without proper input data)
- Task 8: End-to-end verification (cannot verify quality improvement without proper input)
- ALL downstream tasks depend on having proper AOV data

**Evidence**:
- `.sisyphus/evidence/task-1-exr-channels.txt` — full investigation report
- `.sisyphus/evidence/task-1-alias-coverage.txt` — alias mapping verification (PASS, but moot without channels)

---

### Recommendation

**PAUSE PLAN EXECUTION** until both blockers are resolved:

1. **Immediate**: Start Blender with MCP addon to unblock Tasks 2-3
2. **Critical**: Re-render test sequence with proper AOV configuration
3. **Then**: Resume plan execution from Task 2 (or Task 4 if Tasks 2-3 complete successfully)

**Alternative**: If user wants to proceed without Blender MCP, we can:
- Skip Tasks 2-3 verification
- Implement Tasks 4-7 based on UE5 reference (assume MV negation IS needed)
- Document the risk that MV direction assumption may be wrong
- Require user to test and potentially revert if assumption is incorrect

This is a **DECISION POINT** that requires user input.
