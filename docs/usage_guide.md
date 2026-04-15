# Usage Guide

This guide describes how to use DLSS Compositor via its GUI and CLI.

## Using the GUI

The DLSS Compositor GUI provides a powerful and intuitive interface for configuring and monitoring your processing.

### Launching
- **Release Version**: Run `dlss-compositor-gui.exe` from the release folder.
- **Development**: Run `npm run dev` in the `gui/` directory.

### Configuration Options
The GUI is divided into a configuration panel on the left and a processing panel on the right.

#### Paths and Executable
- **Executable Path**: Path to `dlss-compositor.exe`. If the GUI and CLI are in the same folder, this is detected automatically.
- **Input/Output Directory**: Source folder for EXR sequences and the target for processed output.

#### DLSS-SR (Super Resolution & Upscaling)
- **Enable Upscaling**: Toggle spatial upscaling and denoising.
- **Scale Factor**: Choose a multiplier (e.g., 1.5, 2.0). Use 1.0 for denoise-only (DLAA).
- **Quality Mode**: Select between `DLAA`, `MaxQuality`, `Balanced`, `Performance`, and `UltraPerformance`.
- **SR Preset**: Select between `J`, `K`, `L`, or `M`. Default is `L`.

#### DLSS-FG (Frame Generation & Interpolation)
- **Interpolation**: Select `Off`, `2x` (RTX 40+), or `4x` (RTX 50+).
- **Camera Data File**: Required when interpolation is enabled. Points to the `camera.json` file exported from Blender.

#### Output & Advanced Settings
- **Encode Video**: Toggle video encoding (`.mp4`) after processing. Requires FFmpeg.
- **EXR Compression**: Choose between `dwaa`, `dwab`, `zip`, `zips`, `piz`, or `none`.
- **Memory Budget**: GPU memory allocation in GB (default 8 GB).
- **Tonemap Mode**: Choose `pq` (HDR transport encoding, default) or `none`.
- **Output Passes**: Select any additional passes to include in the output EXR.

### Progress and Preview
Once processing starts:
- **Progress Bar**: Shows current progress and total frame count.
- **Frame Preview**: Displays the current frame number, total frames, and the filename of the most recently processed frame.
- **Completion Banner**: A green banner appears upon success, showing total time elapsed and final frame count.

Your settings are automatically saved and will be restored the next time you open the app.

## Blender Workflow

To use DLSS Compositor, you must render your animation from Blender with specific render passes enabled using the DLSS Compositor Blender Extension.

1. **Install Extension**:
   - In Blender 4.2+, go to **Edit > Preferences > Extensions**.
   - Click the down arrow in the top right, select **Install from Disk**, and choose the `dlss-compositor-blender-v0.1.0.zip` file.
2. **Configure Passes**: 
   - Go to the **Render Properties** tab.
   - Look for the **DLSS Compositor** panel.
   - Click **Configure All Passes**. This will automatically enable the required G-buffer passes and set the output format to **OpenEXR MultiLayer**.
3. **Render**: Render your animation sequence to a directory.

## Frame Generation Workflow

Frame interpolation generates intermediate frames between your renders to increase smoothness. It preserves HDR scene-linear values using PQ (ST 2084) transport encoding.

- **Hardware requirement**: RTX 40+ for 2x, RTX 50+ for 4x.
- **Interpolation factor**: `2x` generates 1 intermediate frame per pair. `4x` generates 3 intermediate frames per pair.

### Step 1: Exporting Camera Data from Blender
Frame Generation requires per-frame camera matrices. In the **DLSS Compositor** panel in Blender, click **Export Camera Data**. This exports a `camera.json` file to your configured output directory.

Ensure the frame range in the panel matches your rendered frame range.

### Step 2: Run Interpolation
Use the GUI's **Interpolation** section or run the CLI with the `--interpolate` and `--camera-data` flags:

```bash
dlss-compositor.exe --input-dir renders/ --output-dir output/ --interpolate 2x --camera-data camera.json
```

## Combined Mode (SR + FG)

When both `--scale` and `--interpolate` are specified, the tool runs a combined pipeline: DLSS-SR upscales/denoises each frame first, then DLSS-FG interpolates between the upscaled frames. The SR output stays on the GPU and is handed directly to FG with zero CPU readback — matching how game engines like Unreal Engine chain these features.

```bash
# 2x upscale + 2x interpolation — effective 4x frame count at 2x resolution
dlss-compositor.exe --input-dir renders/ --output-dir output/ --scale 2 --interpolate 2x --camera-data camera.json
```

**Requirements:**
- All requirements for both SR and FG apply (RTX 40+ for 2x FG, RTX 50+ for 4x FG).
- Camera data JSON is required (same as FG-only mode).
- Depth and motion vectors are kept at render resolution — they are not upscaled for FG input.

**Output naming:** Original upscaled frames and interpolated frames are interleaved and numbered sequentially, same as FG-only mode.

## Custom Scale Factors

The `--scale` flag supports any float value between 1.0 and 8.0. This allows for precise matching of target resolutions.

| Target Resolution | Source Resolution | Scale Factor | Example Command |
|-------------------|-------------------|--------------|-----------------|
| 1080p (1920x1080) | 720p (1280x720)   | 1.5          | `--scale 1.5`   |
| 4K (3840x2160)    | 1080p (1920x1080) | 2.0          | `--scale 2.0`   |
| 4K (3840x2160)    | 720p (1280x720)   | 3.0          | `--scale 3.0`   |
| Native Denoise    | Native            | 1.0          | `--scale 1.0 --quality DLAA` |

**Denoise-only (DLAA) Mode:**
By setting `--scale 1.0` and `--quality DLAA`, the compositor performs high-quality temporal denoising and ray reconstruction at the original resolution without any spatial upscaling.

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
| `--scale <factor>` | 2.0 | Upscale factor. Float value ≥ 1.0 (e.g., 1.5, 2.0). Use 1.0 for denoise-only (DLAA). |
| `--quality <mode>` | Balanced | DLSS quality mode: `DLAA`, `MaxQuality`, `Balanced`, `Performance`, `UltraPerformance`. |
| `--preset <mode>` | L | SR preset hint: `J`, `K`, `L`, `M`. |
| `--encode-video [file]` | — | Encode the output sequence to an MP4 video. Requires FFmpeg on your PATH. |
| `--fps <rate>` | 24 | Frame rate for the video encoding. |
| `--interpolate <2x\|4x>` | — | Enable frame interpolation. `2x` generates 1 intermediate frame per pair (RTX 40+). `4x` generates 3 intermediate frames per pair (RTX 50+). |
| `--camera-data <file>` | — | Required when `--interpolate` is used. Path to per-frame camera JSON exported from the Blender panel. |
| `--tonemap <mode>` | pq | FG transport encoding mode: `pq` (PQ ST 2084, default) or `none` (raw scene-linear). |
| `--no-inverse-tonemap` | — | Skip inverse PQ decode on FG output (output stays PQ-encoded). |
| `--tonemap-lut <file>` | — | Custom forward LUT binary file (implies custom LUT mode). |
| `--inverse-tonemap-lut <file>` | — | Custom inverse LUT binary file. |
| `--memory-budget <GB>` | 8 | GPU memory budget in GB for texture preloading and pooling. Minimum 1. |
| `--channel-map <file>` | — | Path to a custom JSON file for channel name mapping. |
| `--test-ngx` | — | Verify DLSS Super Resolution availability on your system and exit. |
| `--help`, `-h` | — | Show the help message. |
| `--version` | — | Show the version number. |

### Video Encoding
If you use `--encode-video`, the tool will call FFmpeg to create a video from the processed EXR frames. Ensure `ffmpeg` is accessible from your command prompt.

## Troubleshooting

### "InReset=true" on every frame
DLSS-SR relies on temporal history. If your filenames are not sequentially numbered (e.g., `frame_01.exr`, `frame_03.exr`), the tool will reset its history on every gap, leading to lower quality. Ensure your sequence is contiguous.

### Output is blurry
- Try setting `--quality MaxQuality`.
- Experiment with different `--preset` values (e.g., `K` or `M`).
- Ensure your motion vectors are correctly rendered in Blender.

### Driver errors
Update your NVIDIA drivers to version 520 or newer. DLSS is a relatively new feature that requires modern drivers.

### Frame Generation: "RTX 40+ required"
User's GPU doesn't support DLSS-G. Frame Generation requires Ada Lovelace (RTX 40) or newer architecture.

### Frame Generation: "At least 2 frames required"
Input directory must contain at least 2 EXR files for interpolation.
