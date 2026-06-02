# GUI Guide — DLSS Compositor v2

## Launch

Double-click `dlss_compositor_gui.exe` (located at `gui/build/windows/x64/runner/Release/`).

---

## Status Bar

The bottom status bar shows:
- **DLSS SDK version** (e.g., `DLSS 310.6.0`)
- **GPU name** (e.g., `NVIDIA GeForce RTX 4090`)
- **FG Max multiplier** (e.g., `FG Max 4x`)
- **Progress** (frame counter + progress bar during processing)

If the status shows "Connecting...", the native plugin is initializing.

---

## Open dlssconfig.json

1. Click **Open dlssconfig.json** button
2. Navigate to your Blender output directory (where you rendered)
3. Select `dlssconfig.json` (generated automatically by the Blender extension after render)

On success, the picker shows summary chips:
- Output resolution (e.g., `3840×2160`)
- Quality mode (e.g., `balanced`)
- HDR status (if applicable)
- FPS and frame count

On failure, a red error message explains the schema violation.

---

## Configure Task

After loading a valid `dlssconfig.json`:

### Preset
- **Auto** (default): DLSS automatically selects the best preset per quality mode
  - DLAA/Quality/Balanced/Ultra Quality → Preset K
  - Performance → Preset M
  - Ultra Performance → Preset L
- **J, K, L, M**: Override to specific preset

### Frame Generation
- **Enable FG toggle**: Enable DLSS Frame Generation (requires RTX 40+ GPU)
- **Multiplier**: 2x / 3x / 4x — buttons above `fg_max_multiplier` are grayed out

### Output Settings
- **Output Directory**: Where to write processed frames
  - Default: same directory as `dlssconfig.json`
- **Filename Template**: `frame_####` (4-digit zero-padded) or `{frame:04d}`
  - Live preview shows "Sample: frame_0001.exr"
- **Output Format**:
  - LDR project: `PNG (16-bit)` (default) / `EXR (scene-linear)` / `ProRes 422 HQ`
  - HDR project: `EXR (scene-linear)` (default) / `ProRes 422 HQ`
- **Overwrite Mode**:
  - `Overwrite` (default): Replace existing files
  - `Skip`: Skip if file exists (logs a warning)
  - `Auto-version (_v##)`: Auto-increment version suffix

---

## Start Processing

Click **Start**. If output files exist that would be affected:
- A confirmation dialog shows the number of conflicting files
- Choose to proceed or cancel

During processing:
- Progress bar updates in the status bar
- Live preview frames appear in the right panel (~3 fps)
- Log messages scroll in the log panel

---

## Done

When processing completes:
- Completion statistics appear (time, frame count, format)
- Click **Open Output Folder** to open the output directory in Explorer

---

## Troubleshooting

**FG multiplier buttons are grayed out**
→ Your GPU doesn't support that multiplier. RTX 30 and earlier don't support FG at all.

**"dlssconfig schema invalid" error**
→ Re-render in Blender with the DLSS extension enabled. The schema error shows the specific field.

**Preview frame not updating**
→ Check the Log panel for errors. FFmpeg or engine errors may prevent preview.

**NGX feature unavailable**
→ Update NVIDIA drivers to 535+. Check `dlss-compositor-cli.exe --self-check`.
