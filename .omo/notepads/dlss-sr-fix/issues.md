# Issues & Gotchas — DLSS-SR Fix

## Problems Encountered

(Agents will append problems and solutions here)

## [2026-04-16T16:53:01Z] Task 8 Retry: MCP disconnect after initial success
- Initial blender_get_scene_info succeeded (blank scene).
- First blender_execute_blender_code failed on file format enum mismatch (OPEN_EXR_MULTILAYER on render.image_settings).
- Subsequent calls failed with connection error: Could not connect to Blender.
- Recommendation: keep Blender app open/foreground, avoid addon restart during long execute call; retry without wm.read_factory_settings if addon transport is fragile.

## [2026-04-17T01:45:00Z] F3 QA: Missing AOV Passes in Task 8 Test Renders

**Status**: CRITICAL ISSUE
**Impact**: Cannot verify DLSS-SR quality improvement

**Details**:
- Task 8 rendered 10 test frames to C:\Users\White\AppData\Local\Temp\dlss_e2e_test\
- dlsscomp.configure_passes() reported success
- dlsscomp.export_camera() generated camera.json with jitter (CORRECT)
- Compositor processed all 10 frames with jitter offsets (CORRECT)
- BUT: Input EXR files only contain 4 channels (Image.RGBA)
- MISSING: Vector.X/Y, Depth.Z, Normal.X/Y/Z

**Root Cause**:
- Configure Passes operator did not enable Blender View Layer passes
- OR compositor File Output node not configured correctly
- OR render output node overrode File Output settings

**Required Fix**:
1. Debug blender/aov_export_preset.py::configure_passes()
2. Verify use_pass_vector, use_pass_z flags are set
3. Verify File Output node connections
4. Re-render and verify channels with OpenEXR

**Evidence**:
- Input EXR: file_name0200.exr (1.6MB, 4 channels only)
- Python check: OpenEXR.InputFile().header()['channels'] = ['Image.A', 'Image.B', 'Image.G', 'Image.R']
- Expected: Should also include Vector.X/Y, Depth.Z, Normal.X/Y/Z
