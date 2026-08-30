# test-frames/

Lightweight dry-run metadata for DLSS pipeline regression tests.

## What lives here

| File | Description |
|---|---|
| `rr_inject_ready.json` | `inject_ready` event from a dry-run RR inject (frames 300–350) |
| `sr_inject_ready.json` | `inject_ready` event from a dry-run SR inject (frames 300–350) |

EXR frame files are **not committed**. The RR test source lives in `test-frames/rr/`
(local only, gitignored binaries) and can also be regenerated onto the RAM disk.

## Regenerating the JSON metadata

```bat
python tools/export_test_frames.py --dry-run --mode rr --output test-frames/rr_inject_ready.json
python tools/export_test_frames.py --dry-run --mode sr  --output test-frames/sr_inject_ready.json
```

## Running a full test export (to RAM disk)

Make sure `R:\` is mounted (ImDisk 8 GB) and Blender 5.3 is available, then:

```bat
python tools/export_test_frames.py --mode rr
```

Frames 300–350 will be exported to `R:\rr-test\` in RR EXR multilayer format.
This is the frame range used for DLSS pipeline integration tests — the blend's
animation is intact in this range (300–350).
