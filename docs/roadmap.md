# Roadmap Notes

Working notes for future DLSS Compositor milestones. Keep this as intent and design context, not a replacement for issues or implementation docs.

## Installer

- Add an MSI installer for Windows releases.
- Installer should place the GUI, native DLSS runtime files, Blender extension package, and required resources in predictable locations.
- Future installer can also own file association for `.dlssc`; until then, manual "open with" remains acceptable.

## Render-To-Video Scratch Storage

- Explore a virtual RAM disk / memory-backed scratch filesystem for Blender AOV output.
- Intended flow:
  - Blender writes AOV EXR frames to the scratch path.
  - DLSS Compositor watches/consumes each frame or small frame batch.
  - Finished frames are pushed into FFmpeg video output.
  - Consumed EXRs are deleted immediately.
- Goals:
  - Reduce SSD write volume and wear.
  - Avoid large intermediate AOV folders, especially for long projects.
  - Support a "direct-to-video" workflow once the DLSS pipeline is stable enough that repeated reruns are uncommon.
- Candidate technology: WinFsp-backed virtual filesystem or another Windows RAM disk/scratch mount.
- Design constraints:
  - Must tolerate Blender or DLSS process crashes without silently losing the whole job state.
  - Needs backpressure so Blender does not outrun DLSS/encoder capacity.
  - Should keep a conventional disk-output fallback for debugging and reproducibility.
