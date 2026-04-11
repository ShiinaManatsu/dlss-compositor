# Usage Guide

This guide describes how to use DLSS Compositor with Blender and as a standalone CLI tool.

## Blender Workflow

To use DLSS Compositor, you must render your animation from Blender with specific render passes enabled.

1. **Setup Script**: Open your Blender scene. In the Scripting tab, open `blender/aov_export_preset.py` and click **Run Script**.
2. **Configure Passes**: 
   - Go to the **Render Properties** tab.
   - Look for the **DLSS Compositor** panel.
   - Click **Configure All Passes**. This will automatically enable "Combined", "Z", "Vector", "Normal", "DiffCol", "GlossCol", and "Roughness" passes and set the output format to **OpenEXR MultiLayer**.
3. **Render**: Render your animation sequence to a directory.

## Frame Generation Workflow

Frame interpolation generates intermediate frames between your renders to increase smoothness. It preserves HDR scene-linear values and uses NVIDIA DLSS-G optical flow.

- **Hardware requirement**: RTX 40+ for 2x, RTX 50+ for 4x.
- **Interpolation factor**: `2x` generates 1 intermediate frame per pair. `4x` generates 3 intermediate frames per pair.
- **Output naming**: Original and interpolated frames are interleaved and re-numbered sequentially (e.g., 0001, 0002, 0003...).

### Step 1: Exporting Camera Data from Blender
Frame Generation requires per-frame camera matrices. Run the export script via Blender's CLI:

```bash
blender --background scene.blend --python blender/export_camera_data.py -- --output camera.json --start 1 --end 250
```

This exports the camera data needed for optical flow. Ensure `--start` and `--end` match your rendered frame range.

### Step 2: Run Interpolation
Run the compositor with the `--interpolate` and `--camera-data` flags:

```bash
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json
```

## Combined Mode (RR + FG)

When both `--scale` and `--interpolate` are specified, the tool runs a combined pipeline: DLSS-RR upscales/denoises each frame first, then DLSS-FG interpolates between the upscaled frames. The RR output stays on the GPU and is handed directly to FG with zero CPU readback — matching how game engines like Unreal Engine chain these features.

```bash
# 2x upscale + 2x interpolation — effective 4x frame count at 2x resolution
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --interpolate 2x --camera-data camera.json
```

**Requirements:**
- All requirements for both RR and FG apply (RTX 40+ for 2x FG, RTX 50+ for 4x FG).
- Camera data JSON is required (same as FG-only mode).
- Depth and motion vectors are kept at render resolution — they are not upscaled for FG input.

**Output naming:** Original upscaled frames and interpolated frames are interleaved and numbered sequentially, same as FG-only mode.

## CLI Usage

The `dlss-compositor.exe` tool processes EXR sequences.

### Basic Command
```bash
dlss-compositor.exe --input-dir "C:/path/to/renders/" --output-dir "C:/path/to/output/" --scale 2
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--input-dir <dir>` | — | Directory containing the input EXR sequence. |
| `--output-dir <dir>` | — | Directory where processed EXRs will be saved. |
| `--scale <factor>` | 2 | Upscale factor. Must be 2, 3, or 4. |
| `--quality <mode>` | Balanced | DLSS quality mode: `MaxQuality`, `Balanced`, `Performance`, `UltraPerformance`. |
| `--encode-video [file]` | — | Encode the output sequence to an MP4 video. Requires FFmpeg on your PATH. |
| `--fps <rate>` | 24 | Frame rate for the video encoding. |
| `--interpolate <2x|4x>` | — | Enable frame interpolation. `2x` generates 1 intermediate frame per pair (RTX 40+). `4x` generates 3 intermediate frames per pair (RTX 50+). |
| `--camera-data <file>` | — | Required when `--interpolate` is used. Path to per-frame camera JSON exported by `export_camera_data.py`. |
| `--memory-budget <GB>` | 8 | GPU memory budget in GB for texture preloading and pooling. Minimum 1. |
| `--channel-map <file>` | — | Path to a custom JSON file for channel name mapping. |
| `--gui` | — | Launch the ImGui viewer for visual inspection. |
| `--test-ngx` | — | Verify DLSS Ray Reconstruction availability on your system and exit. |
| `--help`, `-h` | — | Show the help message. |
| `--version` | — | Show the version number. |

### Video Encoding
If you use `--encode-video`, the tool will call FFmpeg to create a video from the processed EXR frames. Ensure `ffmpeg` is accessible from your command prompt.

## GUI Usage

Launch the GUI using the `--gui` flag.

- **File Loading**: Use the file browser or provide an input directory via CLI.
- **Channel Preview**: View individual passes (Color, Depth, Motion, etc.) to verify they were rendered correctly.
- **Split View**: Drag the slider to compare the original noisy render with the DLSS-RR processed result.
- **Motion Vectors**: Visualizes motion vectors to ensure direction and scale are correct.

## Troubleshooting

### "InReset=true" on every frame
DLSS-RR relies on temporal history. If your filenames are not sequentially numbered (e.g., `frame_01.exr`, `frame_03.exr`), the tool will reset its history on every gap, leading to lower quality. Ensure your sequence is contiguous.

### Output is blurry
- Try setting `--quality MaxQuality`.
- Ensure your motion vectors are correctly rendered in Blender.

### Driver errors
Update your NVIDIA drivers to version 520 or newer. Ray Reconstruction is a relatively new feature that requires modern drivers.

### Frame Generation: "RTX 40+ required"
User's GPU doesn't support DLSS-G. Frame Generation requires Ada Lovelace (RTX 40) or newer architecture.

### Frame Generation: "At least 2 frames required"
Input directory must contain at least 2 EXR files for interpolation.
