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
