# CLI Flag Reference

Complete reference for all `dlss-compositor.exe` command-line flags.

## Input / Output

| Flag | Description | Default |
|------|-------------|---------|
| `--input-dir <dir>` | Input EXR sequence directory | (required) |
| `--output-dir <dir>` | Output EXR sequence directory | (required) |
| `--channel-map <file>` | Custom JSON channel name mapping | — |

## Super Resolution (DLSS-SR)

| Flag | Description | Default |
|------|-------------|---------|
| `--scale <factor>` | Upscale factor (float, 1.0–8.0) | 2.0 |
| `--quality <mode>` | `DLAA` / `MaxQuality` / `Balanced` / `Performance` / `UltraPerformance` | `MaxQuality` |
| `--preset <hint>` | SR preset: `auto` / `J` / `K` / `L` / `M` | `auto` |
| `--sharpness <0.0–1.0>` | Post-DLSS sharpening (0=off, 0.2–0.4 recommended) | 0.0 |
| `--source-fps <rate>` | Source animation frame rate for DLSS temporal analysis | 30 |

## Frame Generation (DLSS-FG)

| Flag | Description | Default |
|------|-------------|---------|
| `--interpolate <mode>` | Frame interpolation: `2x` or `4x` | — |
| `--camera-data <file>` | Camera metadata JSON; auto-discovered from `<input-dir>/camera.json` if not specified | — |

### Camera.json Auto-Discovery

When `--camera-data` is not specified and `--input-dir` contains a `camera.json`, it is used automatically. Info message logged: `[camera] auto-discovered: <path>`.

## Tonemapping (FG HDR Transport)

| Flag | Description | Default |
|------|-------------|---------|
| `--tonemap <mode>` | Transport encoding: `none` / `pq` / `custom` | `pq` |
| `--no-inverse-tonemap` | Skip inverse PQ decode (output stays PQ-encoded) | — |
| `--tonemap-lut <file>` | Forward 3D LUT binary (implies `--tonemap custom`) | — |
| `--inverse-tonemap-lut <file>` | Inverse 3D LUT binary | — |

## EXR Output

| Flag | Description | Default |
|------|-------------|---------|
| `--exr-compression <m>` | `none` / `zip` / `zips` / `piz` / `dwaa` / `dwab` | `dwaa` |
| `--exr-dwa-quality <f>` | DWA compression quality | 95 |
| `--output-passes <list>` | Comma-separated: `beauty`, `depth`, `normals` | `beauty` |

## Video Encoding

| Flag | Description | Default |
|------|-------------|---------|
| `--encode-video [file]` | Encode output to MP4 via FFmpeg | — |
| `--fps <rate>` | Video encoding frame rate | 24 |

## Performance

| Flag | Description | Default |
|------|-------------|---------|
| `--memory-budget <GB>` | GPU texture pool size in GB (min 1) | 8 |

## Diagnostics & Logging

| Flag | Description | Default |
|------|-------------|---------|
| `--progress-format <f>` | Progress output: `human` (default) or `json` (NDJSON stream) | `human` |
| `--ngx-log-level <l>` | NGX SDK verbosity: `off` / `error` / `warn` / `info` / `verbose` | `off` |
| `--test-ngx` | Test DLSS availability and exit | — |
| `--test-vulkan` | Initialize Vulkan compute context and exit | — |
| `--help`, `-h` | Show help | — |
| `--version` | Show version | — |

## NDJSON Event Stream

When `--progress-format json` is specified, stdout outputs newline-delimited JSON events. Human-readable logs continue on stderr.

| Event | Key Fields |
|-------|-----------|
| `started` | `total`, `input_dir`, `output_dir` |
| `frame_started` | `n`, `total`, `timestamp_ms` |
| `frame_written` | `n`, `path`, `timestamp_ms` |
| `banner` | `sdk`, `engine`, `sr_available`, `fg_available`, `max_multi_frame` |
| `error` | `frame?`, `message` |
| `complete` | `total`, `duration_ms`, `errors` |

Example:
```json
{"event":"started","total":120,"input_dir":"renders/","output_dir":"output/"}
{"event":"banner","sdk":"310.5.3","engine":"dlss-compositor 0.2.1","sr_available":true,"fg_available":true,"max_multi_frame":4}
{"event":"frame_started","n":1,"total":120,"timestamp_ms":1234567890}
{"event":"frame_written","n":1,"path":"output/frame_0001.exr","timestamp_ms":1234568000}
{"event":"complete","total":120,"duration_ms":60000,"errors":0}
```
