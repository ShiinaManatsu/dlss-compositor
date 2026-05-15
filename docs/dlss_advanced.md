# DLSS Advanced Settings Reference

This document covers every advanced CLI flag added in the dlss-pipeline-fix
plan (tasks T1+T2+T3+T4). All flags ship with defaults that match the
previous behavior — users who do not set them see no change.

Brief flag summaries also live in [`usage_guide.md`](usage_guide.md) and
[`cli_reference.md`](cli_reference.md). This page focuses on **why** and
**when** to use each knob, not just **what** it does.

## Per-Quality Preset Selection

DLSS-SR selects a rendering preset (algorithm variant) per quality mode.
By default (`--preset auto`), the compositor uses NVIDIA's recommended
presets per the SDK headers (`DLSS/include/nvsdk_ngx_defs.h:82-85`):

| Quality Mode       | Auto Preset | Reason                                    |
|--------------------|-------------|-------------------------------------------|
| DLAA               | K           | Transformer-based, best image quality     |
| MaxQuality         | K           | Transformer-based, best image quality     |
| Balanced           | K           | Transformer-based, best image quality     |
| Performance        | M           | NVIDIA-recommended for Performance mode   |
| UltraPerformance   | L           | NVIDIA-recommended for UltraPerformance   |
| UltraQuality       | K           | Quality-preserving default for hint slot  |

### Flags

| Flag                                               | Default | Example                            |
|----------------------------------------------------|---------|------------------------------------|
| `--preset <auto\|J\|K\|L\|M>`                      | auto    | `--preset K`                       |
| `--preset-dlaa <auto\|J\|K\|L\|M>`                 | auto    | `--preset-dlaa K`                  |
| `--preset-quality <auto\|J\|K\|L\|M>`              | auto    | `--preset-quality J`               |
| `--preset-balanced <auto\|J\|K\|L\|M>`             | auto    | `--preset-balanced K`              |
| `--preset-performance <auto\|J\|K\|L\|M>`          | auto    | `--preset-performance M`           |
| `--preset-ultra-performance <auto\|J\|K\|L\|M>`    | auto    | `--preset-ultra-performance L`     |
| `--preset-ultra-quality <auto\|J\|K\|L\|M>`        | auto    | `--preset-ultra-quality K`         |

### Resolution Priority

For each quality mode the slot value is resolved as:

```
per-quality flag (e.g. --preset-performance) → global --preset → Auto table
```

If both a per-quality override and `--preset` are set, the per-quality
override wins for that slot only; other slots still pick up the global
preset (or fall back to Auto when global is `auto`).

### When to Override

- **Comparing quality vs. performance presets** for a specific quality
  mode without touching the others — use the per-quality flag.
- **Reproducing a known-good UE5.7 DLSS Plugin v8.6.0 configuration** —
  see `DLSSUpscaler.cpp:372-454` in the UE5 source for reference.
- **Pinning a single preset across all modes** to keep results stable
  across CI machines — set `--preset K` (or whichever) globally.

## Advanced NGX Feature Flags

These flags toggle DLSS-SR feature-creation flags. Changing them from the
defaults can degrade quality or produce NGX errors — they exist for A/B
testing and debugging.

| Flag                                                     | Default  | Description                                                                       |
|----------------------------------------------------------|----------|-----------------------------------------------------------------------------------|
| `--feature-flag-hdr` / `--no-feature-flag-hdr`           | enabled  | Toggles `NVSDK_NGX_DLSS_Feature_Flags_IsHDR`. Disable only for sRGB inputs.       |
| `--feature-flag-mv-low-res` / `--no-feature-flag-mv-low-res` | enabled | Toggles `MVLowRes`. Disable only when motion vectors are at output resolution. |
| `--feature-flag-auto-exposure` / `--no-feature-flag-auto-exposure` | enabled | Toggles `AutoExposure`. When OFF, `--exposure-scale` is forwarded to NGX as `InPreExposure`. |
| `--enable-output-subrects` / `--no-enable-output-subrects` | disabled | Toggles `createParams.InEnableOutputSubrects`. Currently unused; exposed for parity. |

## Advanced Eval Parameters

These knobs forward directly into the per-frame `NVSDK_NGX_VK_DLSS_Eval_Params`.

| Flag                                                       | Default | Valid Range      | Description                                                                                                                     |
|------------------------------------------------------------|---------|------------------|---------------------------------------------------------------------------------------------------------------------------------|
| `--exposure-scale <float>`                                 | `1.0`   | `(0.0, 100.0]`   | NGX `InExposureScale` (and `InPreExposure` when AutoExposure is OFF).                                                           |
| `--frame-time-delta-ms <float>`                            | auto    | `> 0.0`          | Override `InFrameTimeDeltaInMsec`. When omitted, derived from `--source-fps` as `1000 / fps`.                                   |
| `--dlss-sharpness <float>`                                 | `0.0`   | `[0.0, 1.0]`     | NGX `InSharpness` (in-DLSS sharpening). Distinct from post-DLSS `--sharpness` (CAS pass).                                       |
| `--enable-dynamic-resolution` / `--no-enable-dynamic-resolution` | disabled | —          | Reserved knob (FG-only future hook). Stored in `AppConfig` but not yet wired into NGX.                                          |

### Notes

- **Two sharpness knobs are intentional.** `--sharpness` runs a separate
  CAS compute pass *after* DLSS. `--dlss-sharpness` asks NGX to apply
  sharpening *inside* its evaluate. Use one or the other; mixing both at
  high values produces over-sharpened output.
- **Frame time delta** matters for DLSS temporal accumulation. The
  default derived from `--source-fps` is correct for most cases. Override
  only when reproducing a fixed temporal cadence (e.g. matching a
  game-engine capture).

## Camera Cut Detection

DLSS-SR uses temporal history for image quality. A camera cut (sudden
large jump between frames) would smear history from the wrong scene
across the cut. Camera cut detection fires `InReset=true` on large
jumps, preventing inter-shot ghosting.

### How It Works

Each frame, the compositor compares the current frame's camera position
and forward vector (loaded from `camera.json`) against the previous
frame:

- **Translation delta**: Euclidean distance in world units between the
  current and previous camera positions.
- **Rotation delta**: Angle (in degrees) between the current and
  previous camera forward vectors.

If either delta exceeds its threshold the frame is treated as a cut and
DLSS history is reset for that evaluate.

### Flags

| Flag                                       | Default        | Description                                          |
|--------------------------------------------|----------------|------------------------------------------------------|
| `--no-camera-cut-detection`                | detection ON   | Disable camera cut detection entirely.               |
| `--camera-cut-translation-threshold <f>`   | `5.0`          | World-unit translation threshold; cut when exceeded. |
| `--camera-cut-rotation-threshold <f>`      | `30.0`         | Rotation threshold in degrees `(0.0, 180.0]`.        |

### When to Disable

If your scene has legitimate fast camera movement (drone chase, roller
coaster, whip pan) that exceeds the thresholds without being an actual
cut, DLSS will reset history mid-shot and lose accumulated detail. Two
options:

1. **Raise the thresholds** to fit the largest legitimate frame-to-frame
   delta in the shot.
2. **Disable detection entirely** with `--no-camera-cut-detection`. Only
   safe when the input sequence has no real cuts.

## NGX Banner Fields

When `--progress-format json` is active, the compositor emits a single
`"type":"banner"` NDJSON event after successful NGX initialization:

```json
{
  "type": "banner",
  "ngx_sdk_version": "1.5.0",
  "ngx_api_version_used": "0x15",
  "ngx_api_version_path": "primary",
  "dlss_dll_version": "3.7.20.0",
  "dlssg_dll_version": "..."
}
```

If the primary NGX API version fails and the compositor falls back to
the previous API revision, a warning event is emitted alongside the
banner:

```json
{
  "type": "warning",
  "code": "ngx_api_fallback",
  "detail": "Primary NGX API version 0x15 failed; succeeded with fallback 0x14"
}
```

### Field Reference

- `ngx_sdk_version` — Compile-time SDK version string from the NGX SDK.
- `ngx_api_version_used` — Hex value of the NGX API version that
  successfully initialized NGX in this run.
- `ngx_api_version_path` — `"primary"` for normal init, `"fallback"`
  when the secondary API version was needed (driver/SDK mismatch
  workaround).
- `dlss_dll_version` — File version of `nvngx_dlss.dll` actually loaded
  in this process. The runtime DLL may be newer than the compile-time
  SDK because NVIDIA drivers ship updated DLLs over the air.
- `dlssg_dll_version` — Same, for `nvngx_dlssg.dll` (Frame Generation).

GUI consumers parse these fields straight out of the NDJSON stream and
display them in the status bar — no log scraping required.
