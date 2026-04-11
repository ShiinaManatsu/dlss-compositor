# Decisions — dlss-compositor

## 2026-04-10 Project Start
- V1 targets Windows MSVC only
- DLSS-RR only denoiser — no fallback
- Sequences only (temporal requirement of DLSS-RR)
- jitter = 0 for V1

## 2026-04-11 EXR backend migration
- Adopt OpenEXR as the sole EXR backend for both reading and writing, replacing tinyexr, to support Blender 5.x multi-part EXR files.
- Preserve the public `ExrReader` interface and keep channel-name compatibility in the reader via aliases instead of changing `ChannelMapper` or other upper layers.

## 2026-04-11 EXR compression support
- Add CLI-configurable EXR compression with DWAA as the default output mode.
- Keep DWA quality as a header attribute (`dwaCompressionLevel`) so DWAA/DWAB can be tuned without affecting other compression modes.

## 2026-04-11 Output pass flag rollout
- Add `--output-passes` now with `beauty` as the default and only currently supported output pass.
- Parse future-facing `depth` and `normals` flags in the CLI, but warn and ignore them at write time until resolution handling for non-beauty passes is designed.
