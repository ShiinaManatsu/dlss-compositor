# Blender Export Guide

This guide describes the Blender extension that ships with DLSS Compositor and the EXR layout produced for the C++ pipeline.

## Installation

The extension is distributed as a folder (`blender/dlss_compositor_aov/`) and as a packaged zip (`dlss-compositor-blender-v<VERSION>.zip` produced by `scripts/package-release.ps1`).

Install via Blender 4.2+:
1. Open **Edit → Preferences → Extensions**.
2. Click the down arrow in the top right and choose **Install from Disk…**.
3. Select either the zip file or the `dlss_compositor_aov/` folder.
4. Enable the extension if it is not already enabled.

The extension targets Blender 4.2 LTS or newer. Earlier Blender versions used the legacy add-on system and are not supported.

## Panel Location

After enabling the extension, the panel appears at:

**Properties → Render Properties → DLSS Compositor**

The panel is only visible when the active render engine is **Cycles**. Switch to Cycles if the panel is missing.

## Panel Controls

| Control | Description |
|---------|-------------|
| **EXR Compression** | Output codec applied to every rendered EXR. Choices: `ZIP` (default, lossless), `PIZ` (lossless, slower, smaller), `DWAA` / `DWAB` (lossy), `NONE`, `ZIPS`. |
| **Render Scale** | `0.1`–`2.0` factor applied to the render resolution at render time. The original resolution is restored after the render finishes. |
| **Configure All Passes** | One-click setup: enables required AOVs (Combined, Depth, Vector), switches output to OPEN_EXR, wires the compositor node tree to write the standard EXR pair. |
| **Export Camera Data** | Writes `camera.json` for the current frame range — only required for the Frame Generation pipeline. |

## Standard EXR Format

The exporter writes **standard single-layer EXR** files (NOT MultiLayer EXR). This is a deliberate choice so the output opens cleanly in RenderDoc, OpenImageIO, EXR-aware compositors, and the C++ pipeline's OpenEXR reader.

For each rendered frame `N`, two files are produced:

| File | Channels | Purpose |
|------|----------|---------|
| `<basename>####.exr`     | `R`, `G`, `B`, `A` (Combined) + `Z` (Depth) | Color + depth |
| `<basename>####.mv.exr`  | `R` = motion X, `G` = motion Y                | Per-pixel motion vectors |

Both files are written in the configured EXR compression format. The `####` placeholder is Blender's standard frame-number padding (set via the Output panel's filename template).

The motion-vector file uses the suffix `.mv.exr` and lives in the same directory as the main file. The C++ CLI looks for the matching `.mv.exr` next to each `<basename>####.exr` automatically — no separate flag is required.

## Workflow

1. **Install the extension** (see Installation).
2. **Switch to Cycles**, configure your scene, sampling, and denoising.
3. Open **Render Properties → DLSS Compositor** and click **Configure All Passes**. Verify in the Output panel that the format is `OpenEXR` and the file extension reads `.exr`.
4. Set your **Output** directory in the standard Blender Output panel (e.g. `//renders/`).
5. (Optional) Choose a non-default **EXR Compression** or **Render Scale**.
6. Render the animation (`Ctrl+F12` or Render → Render Animation). Render scale is applied automatically before each frame and reverted afterwards, so subsequent renders are unaffected.
7. **For Frame Generation only**: click **Export Camera Data**. This writes `camera.json` containing per-frame camera matrices, FOV, and jitter phases. Place or leave this file in the same directory as the EXR sequence so the GUI auto-discovers it.

## Render Advice

- **Enable Cycles OIDN denoising** before export. DLSS-SR is trained on clean game-engine renders; high-frequency path-traced noise degrades upscaling. Configure denoising in **Render Properties → Sampling → Denoise** with OIDN as the denoiser.
- **Disable adaptive sampling** for time-stable noise. Adaptive sampling can produce per-frame noise variance that confuses DLSS's temporal reuse. Use a fixed sample count.
- **Bake procedural textures** before rendering when possible. Procedural noise that re-evaluates per frame produces non-temporally-coherent input that DLSS will smear.
- **Match Source FPS**. Set the GUI's Source FPS to your scene's frame rate (Output Properties → Frame Rate). DLSS uses this to derive temporal frame timing.
- **Halton jitter** is applied automatically by the extension's frame-pre handler. Do not add additional camera jitter — it will accumulate.

## Troubleshooting

| Issue | Fix |
|-------|-----|
| Panel not visible | Switch the render engine to Cycles. Eevee is not supported. |
| `Combined`, `Depth`, or `Vector` AOV missing | Re-click **Configure All Passes**. Some scene templates strip default AOVs. |
| `.mv.exr` files not produced | The compositor node tree was modified manually. Re-run **Configure All Passes** to restore. |
| Render scale not applied | Confirm Render Scale is between `0.1` and `2.0` and that no other extension is overriding the resolution in `frame_change_pre`. |
| `camera.json` missing fields | Re-export with **Export Camera Data** after configuring all passes — the exporter reads jitter state populated by the render handler. |
| Output filenames missing zero-padding | Set the Blender output filename template to use `####` (e.g. `frame_####`), not `#`. The C++ CLI requires zero-padded numeric suffixes to detect sequence gaps. |
| Existing renders fail in the CLI | Confirm files are standard single-layer EXR. MultiLayer EXR from older versions of this extension is no longer supported — re-render with the current extension. |
