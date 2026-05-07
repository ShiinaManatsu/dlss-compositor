# DLSS Compositor GUI Guide

## Overview

The DLSS Compositor GUI provides a streamlined interface for running DLSS Super Resolution and Frame Generation on EXR render sequences from Blender Cycles (or any multi-layer EXR pipeline).

## Getting Started

### First Launch

1. Launch `dlss-compositor-gui.exe` or run `npm run dev` in `gui/` for development.
2. The exe path auto-detects if `dlss-compositor.exe` is in the same folder; otherwise, click the folder icon at the top to configure it.
3. The **NGX Status Bar** at the top shows DLSS SDK version and GPU capability within 2–3 seconds.

### NGX Status Bar

Located at the very top of the window, populated automatically at startup:

| Element | Meaning |
|---------|---------|
| `DLSS SDK 310.5.3` | NVIDIA DLSS SDK version |
| `DLSS-SR ✓` (green) | Super Resolution available on this GPU |
| `DLSS-SR ✗` (red) | SR not available (check GPU/driver) |
| `DLSS-FG ✓ (4x)` (green) | Frame Generation available, max 4x multiplier |
| `DLSS-FG ✗` (gray) | FG not available (requires RTX 40+) |
| `Engine 0.2.1` | dlss-compositor version |
| `Detecting...` | Briefly shown during startup probe |

---

## Processing Workflow

### Step 1: Input & Output Directories

Click **Browse** next to **Input Directory** to select your EXR render folder.

After selection, the GUI automatically:
- Scans for `camera.json` in the directory → fills the Camera Data File field and shows **"✓ Auto-detected camera.json"**
- Reads `render_width` / `render_height` from `camera.json` → populates the source resolution in the Resolution Panel
- Counts EXR files → shows **"Found N EXR files"**

Then select your **Output Directory**.

### Step 2: Resolution Panel

The resolution panel shows: **Source → Target** with computed scale factor.

| Control | Description |
|---------|-------------|
| **Source** (read-only) | Detected from `camera.json` (e.g., "1920 × 1080"), or "Unknown" |
| **Target Preset** | 4K (3840×2160), 1440p (2560×1440), 1080p (1920×1080), DLAA, Custom |
| **Scale Factor** | Auto-computed (shown as "Scale: 2.00×") |
| **Custom W×H** | Shown when "Custom" preset selected |

When source is unknown: a direct Scale Factor input is shown as fallback.

### Step 3: Basic Settings

| Control | Description | Default |
|---------|-------------|---------|
| Enable Upscaling | Toggle DLSS-SR | on |
| Scale Factor | Direct scale (1.0–8.0); overridden by Resolution Panel | 2.0 |
| Quality Mode | DLAA / MaxQuality / Balanced / Performance / UltraPerformance | MaxQuality |
| Preset | SR preset hint J/K/L/M | L |
| **Source FPS** | Animation frame rate of source footage (NOT video FPS). Used for DLSS temporal analysis | 30 |
| Interpolation | 2x or 4x frame generation (requires camera.json) | None |
| Camera Data File | Auto-filled; or browse manually | — |

### Step 4: Advanced Settings (collapsible)

Click **Advanced Settings** to expand:

| Control | Description | Default |
|---------|-------------|---------|
| Memory Budget (GB) | GPU texture pool size | 8 |
| **Sharpness** | Post-DLSS sharpening [0.0–1.0] (0=off, 0.2–0.4 recommended) | 0.0 |
| **NGX SDK Log Verbosity** | off / error / warn / info / verbose | warn |
| EXR Compression | none / zip / zips / piz / dwaa / dwab | dwaa |
| DWA Quality | DWA compression level | 95 |
| Output Passes | beauty / depth / normals | beauty |
| **Tonemapping** | none / pq / **custom** | pq |
| Forward LUT File | Required when tonemap=custom | — |
| Inverse LUT File | Optional when tonemap=custom | — |

### Step 5: Processing

Click **Start** to begin. The Start button is disabled when required fields are missing.

During processing:
- **Progress Bar**: Frame N/M with percentage, gradient fill
- **ETA**: "~2m 30s remaining" (shown after 3+ frames)
- **Live Frame Preview**: Thumbnail of the latest output EXR (via FFmpeg → PNG conversion)
- **Log Panel**: Raw CLI output for debugging

Click **Stop** to cancel at any time.

---

## FAQ

### Why can't I see the live frame preview?

FFmpeg must be installed and on your system PATH. Check with:
```
ffmpeg -version
```
Download from https://ffmpeg.org/download.html. FFmpeg is also required for `--encode-video`.

### How do I get camera.json?

In Blender 4.2+, install the DLSS Compositor extension, open **Render Properties → DLSS Compositor panel**, configure passes, then click **Export Camera Data**. This creates `camera.json` in your render output directory.

### Why does source resolution show "Unknown"?

Either no `camera.json` was found in the input directory, or the file lacks `render_width`/`render_height` fields. You can still use the Scale Factor field directly.

### What's the difference between Source FPS and the video FPS?

- **Source FPS**: The Blender animation frame rate (e.g., 24 fps for film). Used by DLSS for temporal analysis and frame timing
- **FPS (Encode Video section)**: The output video playback rate when exporting to MP4 via FFmpeg

### Custom tonemap requires a LUT file — where do I get one?

See `docs/usage_guide.md` for the Python LUT generator tool included in the repository.

---

## Troubleshooting

| Issue | Solution |
|-------|---------|
| DLSS-SR unavailable | Requires NVIDIA RTX GPU + driver 520+ |
| DLSS-FG unavailable | Requires RTX 40 series or newer |
| No EXR files found | Verify `.exr` files exist in input directory |
| Processing fails at start | Check log for: scale out of range, missing camera.json for interpolation, missing LUT for custom tonemap |
| Live preview not showing | Install FFmpeg and ensure `ffmpeg` is in PATH |
| Status bar shows "Configure exe path" | Set the path to `dlss-compositor.exe` using the folder icon |
