# test-frames/

Retained render sample metadata and local input assets. Automated test suites
and their export scripts were removed on 2026-11-02.

## What lives here

| File | Description |
|---|---|
| `rr_inject_ready.json` | `inject_ready` event from a dry-run RR inject (frames 300–350) |
| `sr_inject_ready.json` | `inject_ready` event from a dry-run SR inject (frames 300–350) |

EXR frame files are **not committed**. The RR render source lives in `test-frames/rr/`
(local only, gitignored binaries) and can also be regenerated onto the RAM disk.

For fresh inputs, use the application's normal Blender export flow with the
source scene and frame range documented in `rr/SOURCE.md`. The JSON files above
are archived snapshots of earlier dry-run events.
