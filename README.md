# DLSS Compositor

> First open-source tool for processing offline Blender Cycles renders through NVIDIA DLSS Ray Reconstruction (DLSS-RR) and DLSS Frame Generation (DLSS-FG) — AI denoising, temporal upscaling, and 2x/4x frame interpolation for animation sequences.

## What It Does
DLSS Compositor reads multi-channel EXR sequences from Blender Cycles, feeds them through NVIDIA DLSS-RR for denoising and upscaling or DLSS-FG for frame interpolation, and outputs high-quality EXR sequences. It's designed to bring the real-time AI power of DLSS to offline rendering workflows.

## Features
- DLSS-RR denoising and upscaling via Vulkan NGX API
- DLSS Frame Generation 2x/4x interpolation via raw NGX Vulkan API (RTX 40+)
- Combined RR+FG pipeline — upscale and interpolate in a single pass with zero-copy GPU handoff
- PQ (ST 2084) transport encoding for Frame Generation — preserves full HDR range beyond DLSS-FG's internal clamp
- Async EXR writer with thread pool for overlapped disk I/O
- Configurable memory budget (`--memory-budget`) for GPU texture preloading and pooling
- Multi-layer EXR reading with Blender channel name mapping
- Motion vector conversion (Blender 4-channel to DLSS-RR 2-channel current to previous)
- Temporal history management with automatic reset on sequence gaps
- CLI batch processing mode
- ImGui viewer with channel preview and before/after comparison
- Blender addon for one-click render pass configuration and camera data export
- Python EXR validator tool
- Custom LUT support for advanced users (forward/inverse 3D LUT files)

## Requirements
- **GPU**: NVIDIA RTX GPU (Turing, Ampere, Ada, or Blackwell architecture — must support DLSS Ray Reconstruction; RTX 40+ required for Frame Generation)
- **OS**: Windows 10/11 (64-bit)
- **Driver**: NVIDIA driver 520 or newer
- **Build tools**: Visual Studio 2022, CMake 3.20 or newer, Vulkan SDK 1.3 or newer
- **Runtime**: Vulkan SDK runtime DLLs on PATH

## Quick Start
1. **Build**: Clone the repository and build using CMake (see [Build Guide](docs/build_guide.md)).
2. **Configure Blender**: Open your scene, go to the Scripting tab, open `blender/aov_export_preset.py` and click **Run Script** to register the DLSS Compositor panel.
3. **Render**: Use the panel to configure passes and render your animation as a MultiLayer EXR sequence.
4. **Process (DLSS-RR)**: Run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --scale 2`.
5. **Process (DLSS-FG)**: In the Blender panel, click **Export Camera Data** to generate `camera.json`, then run `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --interpolate 2x --camera-data camera.json`.
6. **Process (Combined)**: Run both upscale and interpolation together: `dlss-compositor.exe --input-dir <path_to_renders> --output-dir <output_path> --scale 2 --interpolate 2x --camera-data camera.json`.

## CLI Usage
```bash
# Basic processing with 2x upscale
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2

# Max quality mode
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --quality MaxQuality

# Frame interpolation (2x)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json

# Frame interpolation (4x)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 4x --camera-data camera.json

# Combined upscale + interpolation (RR then FG, zero-copy GPU handoff)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --interpolate 2x --camera-data camera.json

# Custom memory budget for texture preloading (default 8 GB, minimum 1 GB)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --memory-budget 4

# Process and encode to video (requires FFmpeg)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --encode-video result.mp4 --fps 24

# Launch the visual comparison tool
dlss-compositor.exe --gui

# --- HDR Transport Encoding for Frame Generation ---

# FG with PQ transport encoding (default — preserves full HDR range)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json

# FG without transport encoding (raw scene-linear through FG — may clamp HDR values)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --tonemap none

# FG with PQ encoding but skip inverse decode (output stays PQ-encoded)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --no-inverse-tonemap

# FG with custom LUT files (advanced)
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json --tonemap-lut my_forward.bin --inverse-tonemap-lut my_inverse.bin
```

See the [Usage Guide](docs/usage_guide.md) for a full list of flags.

### How PQ Transport Works
DLSS Frame Generation internally clamps HDR color buffers to approximately 10 nits. To preserve the full HDR range:
1. **PQ encode** maps scene-linear values into PQ (ST 2084) perceptual space, compressing the full dynamic range into [0, 1] before FG evaluation
2. FG interpolates in PQ space where no clamping occurs
3. **PQ decode** maps the interpolated result back to scene-linear for EXR output

PQ covers 0 to 10,000 cd/m² — more than enough for any physically-based scene.

## GUI Usage
The ImGui-based viewer allows you to inspect individual EXR channels, compare the noisy input with the DLSS-RR output, and verify motion vectors before batch processing.

## Screenshots
<!-- TODO: Add screenshots -->

## License
MIT License. See `LICENSE` for details.
