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
2. **Configure Blender**: In Blender 4.2+, go to **Edit > Preferences > Extensions**. Click the down arrow in the top right, select **Install from Disk**, and choose the `dlss-compositor-blender-v0.1.0.zip` file (or the `blender/dlss_compositor_aov/` folder for development).
3. **Render**: Use the **DLSS Compositor** panel in the **Render Properties** to configure passes and render your animation as a MultiLayer EXR sequence.
4. **Process (GUI)**: Double-click `dlss-compositor-gui.exe` to launch the GUI. Point it to your input directory and click **Start Processing**.
5. **Process (CLI)**: Run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --scale 2`.
6. **Frame Generation**: In the Blender panel, click **Export Camera Data** to generate `camera.json`, then use the GUI or run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --interpolate 2x --camera-data camera.json`.

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

The Electron-based desktop app provides a streamlined, intelligent interface for DLSS processing.

**Quick Start (5 steps):**
1. **Configure Exe Path**: Point to `dlss-compositor.exe` (first run only — auto-detected if in the same folder)
2. **Select Input Directory**: Browse to your EXR sequence folder — `camera.json` is auto-detected and source resolution auto-filled
3. **Select Output Directory**: Choose where to save processed frames
4. **Choose Target Resolution**: Pick 4K, 1440p, 1080p preset or Custom — scale factor auto-computed from source resolution
5. **Click Start**: Monitor real-time progress, ETA, and live frame preview (requires FFmpeg)

**Phase 4 Features:**
- **NGX Status Bar**: Shows DLSS SDK version and SR/FG availability at app top
- **Auto Camera Detection**: Input directory selection auto-finds `camera.json` and reads source resolution
- **Resolution Presets**: 4K/1440p/1080p/Custom target with automatic scale calculation
- **Live Frame Preview**: FFmpeg converts output EXR frames to PNG thumbnails in real time
- **ETA Estimation**: Estimated time remaining after 3+ frames processed
- **New Controls**: Source FPS, Sharpness (post-DLSS), NGX Log Level, Tonemap=custom mode
- **NDJSON Progress**: Machine-readable event stream for reliable GUI progress parsing

- **Configuration Panel**: Set your executable path, input/output directories, and DLSS parameters (Upscaling factor, Quality mode, Interpolation, etc.).
- **Progress Monitoring**: Real-time progress bar, ETA, and frame-by-frame live preview.
- **Auto-save**: Your settings are automatically persisted between sessions.

Launch by running `dlss-compositor-gui.exe` (found in the release package) or by running `npm run dev` in the `gui/` directory for development.

See [GUI Guide](docs/gui_guide.md) for the full walkthrough.

## Screenshots
<!-- TODO: Add screenshots -->

## License
MIT License. See `LICENSE` for details.
