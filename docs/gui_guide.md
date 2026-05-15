# GUI User Guide

## Overview

The DLSS Compositor GUI is an Electron desktop application that wraps the `dlss-compositor.exe` CLI. It provides a configuration form, NDJSON-driven progress monitoring, and a live frame preview for offline DLSS Super Resolution and Frame Generation runs on EXR sequences from Blender Cycles (or any standard EXR pipeline).

The GUI does not replicate every CLI flag — fields map one-to-one to flags surfaced through `electron/cli-args.ts`. For the full flag list, see [`docs/cli_reference.md`](cli_reference.md). For low-level NGX tuning (per-quality presets, feature flags), see [`docs/dlss_advanced.md`](dlss_advanced.md).

## First Launch

On first launch the GUI auto-detects `dlss-compositor.exe`:
- **Dev mode** (started via `npm run dev`): looks in `build/Release/dlss-compositor.exe` relative to the project root.
- **Packaged**: looks for `dlss-compositor.exe` in the same directory as `dlss-compositor-gui.exe`.

There is no executable path picker. If the binary is missing, the StatusBar shows an explicit error and the **Start** button stays disabled. Build the C++ CLI (`cmake --build build --config Release`) or place the executable next to the GUI to recover.

Once the binary is found, the GUI runs an NGX capability probe (parsed from the `[NGX] …` banner lines on stdout). The StatusBar populates with DLSS SDK version, the bundled `nvngx_dlss.dll` version, SR availability, and FG availability, typically within 2–3 seconds.

## Layout

The window is a single page with three vertical regions:

1. **StatusBar** — top strip; reports DLSS SDK version, SR/FG capability, and any auto-detect errors.
2. **Configuration form** — Input/Output Panel, then three settings sections (Common, SR, FG). All fields update the in-memory config store and save to disk on change.
3. **ProcessingView** — bottom panel; shows the **Start**/**Stop** controls, progress bar, ETA, frame counter, live preview, and raw log.

## Common Settings

The Common Settings section holds options that apply to every pipeline (SR, FG, or SR+FG):

- **Memory Budget (GB)** — GPU texture pool size for preloading. Maps to `--memory-budget`. Default `8`, minimum `1`. Lower values reduce VRAM pressure at the cost of more disk reads per frame.
- **EXR Compression** — output compression codec: `none`, `zip`, `zips`, `piz`, `dwaa`, `dwab`. `dwaa` and `dwab` are lossy and expose a quality slider (default `95`).
- **Output Passes** — comma-separated list of passes to write per frame (`beauty`, `depth`, `normals`). Defaults to `beauty` only.
- **Encode Video** — optional MP4 path. Maps to `--encode-video <file.mp4> --fps <N>`. Requires FFmpeg on `PATH`.
- **NGX SDK Log Verbosity** — `off` / `error` / `warn` / `info` / `verbose`. Maps to `--ngx-log-level`. Default `warn`. Use `verbose` only when filing a bug — it produces large logs.

## SR Settings

SR Settings configure DLSS Super Resolution. The whole section is gated by an **Enable Upscaling** checkbox; when unchecked, SR-only fields are disabled but FG can still be enabled below.

- **Resolution Panel** — see the dedicated subsection.
- **Quality Mode** — `DLAA` / `MaxQuality` / `Balanced` / `Performance` / `UltraPerformance`. Maps to `--quality`. `DLAA` runs at native resolution.
- **Preset** — `auto` / `J` / `K` / `L` / `M`. `auto` uses the NVIDIA-recommended preset for the chosen quality mode.
- **Source FPS** — animation frame rate of the source footage (NOT video output FPS). Maps to `--source-fps`. Used by NGX to derive `InFrameTimeDeltaInMsec = 1000 / fps`. Default `30`.
- **Sharpness** — post-DLSS CAS sharpening, `0.0`–`1.0`. Maps to `--sharpness`. `0` disables. `0.2`–`0.4` is a sensible range.
- **Advanced NGX Panel** — see below.

### Resolution Panel

The Resolution Panel is **aspect-locked** — you cannot change the picture aspect ratio. This prevents accidental anamorphic stretches and matches the constraint that DLSS upscaling is uniform along both axes.

**When source resolution is detected** (from EXR header):
- A single Width or Height input is editable; editing one updates the other automatically using the source aspect ratio.
- The Scale input syncs bidirectionally with the W/H pair — typing `2.0` sets the target to source × 2 and rounds with `Math.floor()` to avoid fractional pixels.
- The preset dropdown shows orientation-aware entries: `4K` / `1440p` / `1080p` for landscape sources, rotated equivalents (`2160×3840`, `1440×2560`, `1080×1920`) for portrait, and square variants when source aspect is exactly `1:1`. Selecting a preset fills the target dimensions and re-derives the scale.
- All computed dimensions floor to integer pixels.

**When source is unknown** (empty input directory or no EXR files):
- Only the manual Scale input is shown. Width/Height inputs and the preset dropdown are disabled.
- A warning in the InputOutputPanel explains why detection failed.

### Advanced NGX Panel (Power User)

The Advanced NGX panel is a collapsible section inside SR Settings. It exposes per-feature NGX tuning that should not be needed for typical workflows. Open it only when you have a reproducible artifact you are trying to address.

Controls include:
- **Per-quality preset overrides** — pin a specific preset (`J`/`K`/`L`/`M`) per quality mode, overriding the `auto` mapping.
- **Feature flags** — `IsHDR`, `MVLowRes`, `MVJittered`, `DepthInverted`, `AutoExposure`, etc. Each maps to a bit in the NGX feature flag mask.
- **Eval overrides** — `Sharpness`, `MVScaleX`/`MVScaleY`, `Exposure`, `PreExposure`.
- **Camera cut detection** — toggles automatic `InReset=true` injection when consecutive camera matrices in `camera.json` differ above a threshold.

For the full mapping from NGX flags to CLI options, see [`docs/dlss_advanced.md`](dlss_advanced.md).

## FG Settings

FG Settings configure DLSS Frame Generation. The section is gated by the Interpolation dropdown and is only available on RTX 40+ GPUs (the StatusBar will show `DLSS-FG ✗` otherwise).

- **Interpolation** — `none` / `2x` / `4x`. Maps to `--interpolate`. `4x` requires Ada or newer.
- **Camera Data File** — path to `camera.json` exported from Blender. Required when interpolation is enabled. The GUI auto-discovers `camera.json` in the input directory but you can override.
- **Tonemap** — `pq` (default) / `none` / `custom`. Maps to `--tonemap`. PQ encoding preserves the full HDR range across FG's internal 10-nit clamp.
- **Forward LUT File** / **Inverse LUT File** — required when `tonemap=custom`. Map to `--tonemap-lut` and `--inverse-tonemap-lut`. See `tools/generate_pq_lut.py` for generating LUTs.
- **Skip Inverse Decode** — corresponds to `--no-inverse-tonemap`. Output stays PQ-encoded; rarely useful outside debugging.

## Source Resolution Auto-Detection

When you select an input directory, the GUI:
1. Scans the directory for `.exr` files and sorts them alphabetically.
2. Picks the first file and runs `dlss-compositor.exe --inspect-exr <path>` over IPC.
3. Parses the printed `width=<W> height=<H>` line from the CLI's stdout and stores it in the config store.
4. Counts total `.exr` files for display.
5. Auto-discovers `camera.json` if present (used as the default value for the FG Settings camera data field).

**Why EXR header instead of `camera.json`**: `camera.json` records the render configuration, including the `render_scale` factor that Blender applies before render. A 1920×1080 render with `render_scale = 0.5` actually produces 960×540 EXR files. Reading the actual EXR header avoids this off-by-scale mismatch.

If detection fails (unreadable EXR, empty directory, missing CLI binary), the InputOutputPanel shows a warning, the Resolution Panel switches to scale-only mode, and processing can still proceed — DLSS will derive the input resolution at runtime.

## Live Preview

After each frame is written, the GUI calls `frame:preview-png` to convert the latest output EXR to a PNG via FFmpeg and display it in the ProcessingView preview pane.

FFmpeg failures are surfaced as structured error messages and do not abort processing:

| Error code | Meaning |
|------------|---------|
| `ffmpeg-not-found` | `ffmpeg` is not on `PATH`. The preview pane shows an install hint; pipeline continues. |
| `decode-error` | FFmpeg exited non-zero on the EXR. Often indicates a partial write; the next frame typically succeeds. |
| `timeout` | FFmpeg ran longer than the per-frame timeout (default 5 s). Common with very large EXRs at low memory. |

The preview is purely informational. Disable it by removing FFmpeg from `PATH` if you do not want the per-frame conversion overhead.

## Settings Persistence

All settings are saved to `gui/dlss-compositor.json` in the GUI directory. This is a project-relative file (NOT Electron's per-user `userData` directory), so the same checkout reuses the same settings across all developers and machines. Version-control this file if you want shared defaults; `.gitignore` it if your team has divergent local paths.

A template with all defaults is committed as `gui/dlss-compositor.example.json`. To reset settings, delete `gui/dlss-compositor.json` and restart the GUI — it will fall back to in-code defaults from `src/types/dlss-config.ts` (`DEFAULT_CONFIG`) or you can copy the example file in by hand.

The store is a flat JSON object — manual edits with a text editor are supported. Malformed JSON is detected at startup and replaced by defaults with a warning in the StatusBar.

## Troubleshooting

| Issue | Fix |
|-------|-----|
| StatusBar shows "CLI not found" | Build the C++ CLI (`cmake --build build --config Release`) or place `dlss-compositor.exe` alongside `dlss-compositor-gui.exe`. |
| StatusBar shows "DLSS-SR ✗" | Requires NVIDIA RTX (Turing+) and driver 535.104+. Verify with `dxdiag` or `nvidia-smi`. |
| StatusBar shows "DLSS-FG ✗" | Requires RTX 40 series or newer. SR can still be used. |
| Source resolution shows "Unknown" | First EXR file failed to decode. Verify `.exr` files exist in the input directory and are not zero-byte. The Scale input still works as a fallback. |
| Preview pane shows `ffmpeg-not-found` | Install FFmpeg from https://ffmpeg.org/download.html and ensure `ffmpeg` is on `PATH`. |
| Preview pane shows `decode-error` | Often a transient partial write; the next frame should display correctly. Persistent errors usually mean an upstream EXR write was truncated. |
| Frame counter advances but no output files | Check **Output Directory** is writable. The CLI log panel at the bottom shows write errors. |
| Settings reset on restart | `gui/dlss-compositor.json` is malformed. Delete it (or replace with `gui/dlss-compositor.example.json`) and restart. |
| Filename pattern not detected | Sequences must use a numeric suffix like `0001.exr`, `frame_0001.exr`, or `render.0001.exr`. Non-numeric or unpadded names will be rejected — rename them in Blender's output template. |
