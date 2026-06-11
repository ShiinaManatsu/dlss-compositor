# CLI Reference — DLSS Compositor v2

## Synopsis

```
dlss-compositor-cli.exe [COMMAND] [OPTIONS]
```

---

## Commands

### --config \<path\>

Process frames using a `dlssconfig.json` config file.

```powershell
dlss-compositor-cli.exe --config renders/dlssconfig.json --output-dir out/
```

**Required**: `--config` or `--rpc-stdio`.

---

### --rpc-stdio

Start JSON-RPC server mode (LSP Content-Length framing). Used by the Flutter GUI plugin as fallback and for automation scripting.

```powershell
# Send a getStatus request
echo '{"jsonrpc":"2.0","method":"getStatus","id":1}' | dlss-compositor-cli.exe --rpc-stdio
```

See [JSON-RPC Methods](#json-rpc-methods) below.

---

### --self-check

Print GPU and DLSS NGX availability report.

```powershell
dlss-compositor-cli.exe --self-check
```

Output:
```
[NGX] DLSS SR: AVAILABLE | FG: AVAILABLE max=4x | nvngx_dlss.dll v310.6.0 | nvngx_dlssg.dll v310.6.0
[GPU] NVIDIA GeForce RTX 4090 (driver 560.0)
```

---

### --version

Print version string.

```powershell
dlss-compositor-cli.exe --version
# dlss-compositor-cli v2.0.0 (DLSS SDK 310.6.0)
```

### --help

Print usage summary.

---

## Options (used with --config)

| Flag | Default | Description |
|------|---------|-------------|
| `--output-dir <path>` | same as config dir | Directory for output frames |
| `--output-format <fmt>` | `exr` (HDR) / `png` (LDR) | `png`, `exr`, `prores_422hq` (LDR only) |
| `--output-template <tmpl>` | `frame_####` | Filename template (`####` = zero-padded frame, `{frame:04d}` = same) |
| `--overwrite-mode <mode>` | `overwrite` | `skip`, `overwrite`, `auto-version` |
| `--preset <preset>` | `auto` | `auto`, `J`, `K`, `L`, `M` |
| `--fg <mult>` | `none` | `none`, `2x`, `3x`, `4x` — enable Frame Generation |
| `--memory-budget <GB>` | `8` | GPU texture pool budget in GB |
| `--rpc-stdio` | — | Use JSON-RPC stdio mode instead of direct processing |

### Examples

```powershell
# SR only, default EXR output
dlss-compositor-cli.exe --config dlssconfig.json --output-dir out/

# LDR project with PNG output, auto-version on conflict
dlss-compositor-cli.exe --config dlssconfig.json --output-dir out/ --output-format png --overwrite-mode auto-version

# Preset K override, specific output template
dlss-compositor-cli.exe --config dlssconfig.json --preset K --output-template "frame_{frame:04d}"
```

### Not Supported (Removed in v2)

The following v1 flags are **not** available in v2 (configuration moved to `dlssconfig.json`):
- `--scale` — upscale ratio is determined by Quality Mode in dlssconfig
- `--quality` — quality mode is set in Blender extension, stored in dlssconfig
- `--inspect-exr` — use Python with OpenEXR directly for EXR inspection
- `--encode-video` — video encoding is not implemented yet
- `--sharpness` — DLSS SR §3.11 deprecated InSharpness; v2 omits this option

---

## JSON-RPC Methods

When using `--rpc-stdio`, the CLI accepts JSON-RPC 2.0 messages on stdin and writes responses to stdout. Messages use `Content-Length` framing (LSP protocol):

```
Content-Length: 42\r\n\r\n{"jsonrpc":"2.0","method":"getStatus","id":1}
```

All diagnostic output goes to stderr and the rotating log file. stdout is exclusively reserved for JSON-RPC framed responses.

### Available Methods

| Method | Params | Returns |
|--------|--------|---------|
| `setTask` | `{config: path, ...}` | `{ok, task_id}` |
| `checkReady` | `{}` | `{ready, reason?}` |
| `checkOutputCollisions` | `{output_dir, template, frame_count}` | `{collisions, files}` |
| `startTask` | `{task_id}` | `{ok}` |
| `getStatus` | `{}` | `{dlss_version, gpu_name, video_encoders, ...}` |
| `getTaskState` | `{}` | `{state, current_frame, total_frames, eta_seconds}` |
| `getProgress` | `{}` | `{current_frame, total_frames, percent, state}` |

### Notes on getStatus

`getStatus` returns only implemented output formats in `video_encoders`. Future format placeholders are not included.

---

## Implemented Output Formats

| Format | Flag | Notes |
|--------|------|-------|
| EXR (scene-linear) | `--output-format exr` | R16G16B16A16 HALF, full HDR range |
| PNG (16-bit) | `--output-format png` | LDR projects only |
| ProRes 422 HQ | `--output-format prores_422hq` | LDR projects only, single `.mov` output |

---

## Future Extensions (Not Implemented in v2)

The following formats are reserved for future releases and are not available in v2:
- DNxHR
- ProRes 4444 XQ
- JPEG / AVIF
- H.264 / H.265 / AV1
- WebP

---

## Logging

The CLI writes logs to the project root as `dlss-compositor-cli.log` (rotating, 5 MB max, 3 files). The log is cleared when the CLI process starts. All log output also goes to stderr during interactive use; stdout is reserved for JSON-RPC `Content-Length` frames.

To increase verbosity, set the environment variable:

```powershell
$env:DLSS_COMPOSITOR_LOG = "debug"
dlss-compositor-cli.exe --config dlssconfig.json --output-dir out/
```

Levels: `trace`, `debug`, `info` (default), `warn`, `error`.
