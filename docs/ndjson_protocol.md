# NDJSON Protocol Reference

The DLSS Compositor CLI emits newline-delimited JSON (NDJSON) on **stdout** when `--progress-format json` is active.

## Stream Routing

- **stdout**: All NDJSON progress and diagnostic events when `--progress-format json` is active.
- **stderr**: Human-readable log lines and NGX/Vulkan SDK output. The GUI treats stderr as log text, not structured progress.

## Event Types

### `started`

Emitted at the beginning of sequence processing.

```json
{"event":"started","total":200,"input_dir":"/path/to/frames","output_dir":"/path/to/output","timestamp_ms":1234567890}
```

### `frame_started`

Emitted when a frame begins processing.

```json
{"event":"frame_started","n":1,"total":200,"timestamp_ms":1234567890}
```

### `frame_written`

Emitted when a frame is written to disk.

```json
{"event":"frame_written","n":1,"path":"/path/to/output/frame_0001.exr","timestamp_ms":1234567890}
```

### `complete`

Emitted when all frames are processed.

```json
{"event":"complete","total":200,"duration_ms":45000,"errors":0,"timestamp_ms":1234567890}
```

### `error`

Emitted on processing errors. Older processing-path errors use `event` and `message`; newer scan errors use `type`, `reason`, and `detail`.

```json
{"event":"error","frame":5,"message":"error description","timestamp_ms":1234567890}
```

```json
{"type":"error","reason":"no_valid_frames","detail":"No EXR files with valid frame numbers found in: /path/to/frames"}
```

### `skipped_file`

Emitted when a file is skipped, for example when an EXR filename has no trailing frame number.

```json
{"type":"skipped_file","path":"/path/to/filename_.exr","reason":"no_trailing_frame_number"}
```

### `camera_cut`

Emitted when camera cut detection resets temporal history.

```json
{"type":"camera_cut","frame":42,"translation_delta":12.5,"rotation_delta_deg":35.2,"timestamp_ms":1234567890}
```

### `banner`

Emitted by `--test-ngx` and during normal initialization after NGX initializes successfully.

```json
{
  "type": "banner",
  "ngx_sdk_version": "310.5.3",
  "ngx_api_version_used": "0x15",
  "ngx_api_version_path": "primary",
  "dlss_dll_version": "310.5.3.0",
  "dlssg_dll_version": "310.5.3.0"
}
```

Legacy GUI-era binaries may emit a banner with an `event` discriminator and availability booleans:

```json
{"event":"banner","sdk":"310.5.3","engine":"0.2.1","sr_available":true,"fg_available":false,"max_multi_frame":0}
```

### `warning`

Emitted for non-fatal conditions.

```json
{"type":"warning","code":"ngx_api_fallback","detail":"Primary NGX API version 0x15 failed; succeeded with fallback 0x14"}
```

## Schema Notes

- Events added before Plan 1 use `"event"` as the discriminator field.
- Events added in Plan 1 and later use `"type"` as the discriminator field.
- The GUI handles both discriminator fields for backward compatibility.
- The current NGX banner uses `"type":"banner"`; consumers must not require `"event":"banner"`.
- In JSON progress mode, stdout is reserved for NDJSON. Human-readable logs are routed to stderr.
