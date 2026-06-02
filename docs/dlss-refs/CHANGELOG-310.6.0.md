# DLSS 310.6.0 changelog notes

Upgrade target: `DLSS` submodule `v310.5.3` -> `v310.6.0`

## Confirmed header/API changes

1. **Removed `pNoPostProcessingColor` from DLSSG helpers**
   - `include/nvsdk_ngx_helpers_dlssg_vk.h` replaces `pNoPostProcessingColor` with `pUIAlpha`.
   - `include/nvsdk_ngx_helpers_dlssg.h` makes the same D3D12-side replacement.

2. **Added create-time resource availability flags**
   - `include/nvsdk_ngx_defs_dlssg.h` adds `NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags`.
   - `include/nvsdk_ngx_defs_dlssg.h` also adds `NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags`.
   - New `NVSDK_NGX_DLSSG_ResourceFlags` enum documents per-resource bitmasks for these flags.

3. **Preset E status changed**
   - `include/nvsdk_ngx_defs.h` no longer says `NVSDK_NGX_DLSS_Hint_Render_Preset_E` is removed.
   - `Preset_E` is now present again as enum value `5` and marked `Deprecated`.

4. **Expanded DLSSG UI/input-output parameter model**
   - `include/nvsdk_ngx_defs_dlssg.h` adds `UIAlpha`, `OutputInterpolated`, and `OutputReal` DLSSG parameter names and documents User Interface Recomposition requirements.
   - `include/nvsdk_ngx_params_dlssg.h` adds `uiAlphaSubrect*`, `outputInterpSubrect*`, and `outputRealSubrect*` fields.
   - Helper code now writes `InputBackbufferSubrect*`, `OutputInterpolatedSubrect*`, and `OutputRealSubrect*` instead of only the old shared backbuffer subrect names.

5. **Additional DLSSG create/eval knobs were added**
   - `include/nvsdk_ngx_defs_dlssg.h` adds `NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled`.
   - `include/nvsdk_ngx_defs_dlssg.h` adds `NVSDK_NGX_DLSSG_Parameter_FullscreenMode` and `NVSDK_NGX_DLSSG_Parameter_TargetFrameRate`.

## Verification summary

- `git -C DLSS describe --tags` => `v310.6.0`
- `pNoPostProcessingColor` no longer appears under `DLSS/include/`
- `ResourceAlwaysProvided_Flags` appears in `DLSS/include/nvsdk_ngx_defs_dlssg.h`
