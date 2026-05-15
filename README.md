# DLSS Compositor

> First open-source tool for processing offline Blender Cycles renders through NVIDIA DLSS Super Resolution (DLSS-SR) and DLSS Frame Generation (DLSS-FG) — AI-powered upscaling, denoising, and 2x/4x frame interpolation for animation sequences.

## What It Does
DLSS Compositor reads multi-channel EXR sequences from Blender Cycles, feeds them through NVIDIA DLSS-SR for upscaling and denoising or DLSS-FG for frame interpolation, and outputs high-quality EXR sequences. It's designed to bring the real-time AI power of DLSS to offline rendering workflows.

## Features
- DLSS-SR upscaling and denoising via Vulkan NGX API
- DLSS Frame Generation 2x/4x interpolation via raw NGX Vulkan API (RTX 40+)
- Combined SR+FG pipeline — upscale and interpolate in a single pass with zero-copy GPU handoff
- PQ (ST 2084) transport encoding for Frame Generation — preserves full HDR range beyond DLSS-FG's internal clamp
- Async EXR writer with thread pool for overlapped disk I/O
- Configurable memory budget (`--memory-budget`) for GPU texture preloading and pooling
- Multi-layer EXR reading with Blender channel name mapping
- Motion vector conversion (Blender 4-channel to DLSS-SR 2-channel current to previous)
- Temporal history management with automatic reset on sequence gaps
- CLI batch processing mode
- Electron-based GUI for easy configuration and progress monitoring
- Blender extension for one-click render pass configuration and camera data export
- Python EXR validator tool
- Custom LUT support for advanced users (forward/inverse 3D LUT files)

## Requirements
- **GPU**: NVIDIA RTX GPU (Turing, Ampere, Ada, or Blackwell architecture; RTX 40+ required for Frame Generation)
- **OS**: Windows 10/11 (64-bit)
- **Driver**: NVIDIA driver 520 or newer
- **Build tools**: Visual Studio 2022, CMake 3.20 or newer, Vulkan SDK 1.3 or newer
- **Runtime**: Vulkan SDK runtime DLLs on PATH

## Quick Start
1. **Build**: Clone the repository and build using CMake (see [Build Guide](docs/build_guide.md)).
2. **Install Blender extension**: In Blender 4.2+, go to **Edit > Preferences > Extensions**. Click the down arrow in the top right, select **Install from Disk**, and choose the `dlss-compositor-blender-v<VERSION>.zip` file (or the `blender/dlss_compositor_aov/` folder for development).
3. **Render**: Open **Render Properties → DLSS Compositor**. Click **Configure All Passes**, set output directory, render the sequence. The exporter writes standard single-layer EXR (`####.exr` for color/depth, `####.mv.exr` for motion vectors). See [Blender Export Guide](docs/blender_export_guide.md).
4. **Open the GUI**: Double-click `dlss-compositor-gui.exe`. The CLI binary is auto-detected from `build/Release/` in dev or alongside the packaged GUI executable. Select the input directory — the GUI inspects the first EXR's header and fills source resolution automatically.
5. **Process (GUI)**: Configure target resolution, quality, and output directory, then click **Start**.
6. **Process (CLI)**: Run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --scale 2`.
7. **Frame Generation**: In the Blender panel, click **Export Camera Data** to generate `camera.json`, then enable Interpolation in the GUI's FG Settings or run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --interpolate 2x --camera-data camera.json`.

## CLI Usage
```bash
# Basic processing with 2x upscale
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2

# Custom float scale (e.g. 720p to 1080p is 1.5x)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 1.5

# Denoise-only mode (1.0x scale with DLAA)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 1.0 --quality DLAA

# Max quality mode
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --quality MaxQuality

# Set SR preset (J, K, L, M; default: L)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --preset K

# DLAA quality mode (superior denoising at native resolution)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 1.0 --quality DLAA

# Frame interpolation (2x)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json

# Frame interpolation (4x)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 4x --camera-data camera.json

# Combined upscale + interpolation (SR then FG, zero-copy GPU handoff)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --interpolate 2x --camera-data camera.json

# Custom memory budget for texture preloading (default 8 GB, minimum 1 GB)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --memory-budget 4

# Process and encode to video (requires FFmpeg)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --encode-video result.mp4 --fps 24

# --- HDR Transport Encoding for Frame Generation ---

# FG with PQ transport encoding (default — preserves full HDR range)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json

# FG without transport encoding (raw scene-linear through FG — may clamp HDR values)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --tonemap none

# FG with PQ encoding but skip inverse decode (output stays PQ-encoded)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --no-inverse-tonemap

# FG with custom LUT files (advanced)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --tonemap-lut my_forward.bin --inverse-tonemap-lut my_inverse.bin

# --- Phase 4 New Flags ---

# Machine-readable NDJSON output (for scripting/automation)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --progress-format json

# Set source animation frame rate (for accurate DLSS temporal analysis)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --source-fps 24

# Post-DLSS sharpening (0.2-0.4 recommended)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --sharpness 0.3

# Camera.json auto-discovery (no --camera-data needed if camera.json is in input dir)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x

# Custom tonemap LUT files
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --tonemap custom --tonemap-lut my_forward.bin --inverse-tonemap-lut my_inverse.bin
```

See the [Usage Guide](docs/usage_guide.md) for a full list of flags.

### How PQ Transport Works
DLSS Frame Generation internally clamps HDR color buffers to approximately 10 nits. To preserve the full HDR range:
1. **PQ encode** maps scene-linear values into PQ (ST 2084) perceptual space, compressing the full dynamic range into [0, 1] before FG evaluation
2. FG interpolates in PQ space where no clamping occurs
3. **PQ decode** maps the interpolated result back to scene-linear for EXR output

PQ covers 0 to 10,000 cd/m² — more than enough for any physically-based scene.

## GUI Usage

The Electron-based desktop application provides a configuration and monitoring interface for the DLSS Compositor CLI.

**CLI auto-detection**: The CLI binary is auto-detected from `build/Release/dlss-compositor.exe` relative to the project root in dev mode, or from `dlss-compositor.exe` placed alongside `dlss-compositor-gui.exe` in a packaged release. There is no executable path picker — if detection fails, the StatusBar shows an explicit error.

**Settings persistence**: All settings persist to `gui/dlss-compositor.json` in the GUI directory (project-relative, not Electron `userData`). A template `gui/dlss-compositor.example.json` is committed with all defaults.

**Quick Start (5 steps):**
1. **Install + render** in Blender via the DLSS Compositor extension (see [Blender Export Guide](docs/blender_export_guide.md)).
2. **Open the GUI**: Run `dlss-compositor-gui.exe`, or `npm run dev` from `gui/` for development. The StatusBar reports DLSS SDK version, SR availability, and FG availability after the auto-probe completes.
3. **Select Input Directory**: The GUI runs `dlss-compositor.exe --inspect-exr` against the first `.exr` file alphabetically and reads source resolution from its header. The Resolution Panel switches into aspect-locked mode with single W or H + Scale inputs and orientation-aware presets.
4. **Configure**: Three sections — **Common Settings** (memory budget, EXR compression, video output), **SR Settings** (Resolution Panel, quality mode, preset, source FPS, sharpness, plus a collapsible Advanced NGX panel), **FG Settings** (interpolation, camera data, tonemap, LUT files).
5. **Start**: Monitor real-time progress, ETA, and live frame preview. FFmpeg is required for the live preview and for `--encode-video`; missing or failed FFmpeg surfaces a structured error in the preview pane and the rest of the pipeline continues unaffected.

See [GUI Guide](docs/gui_guide.md) for the full walkthrough and [DLSS Advanced](docs/dlss_advanced.md) for per-quality presets and feature flag tuning.

## License
MIT License. See `LICENSE` for details.
