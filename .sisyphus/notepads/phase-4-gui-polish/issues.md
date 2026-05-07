# Phase 4 GUI Polish - Issues

## 2026-05-07 Initial Issues

### Known Gaps to Address
1. `--source-fps`, `--sharpness`, `--ngx-log-level` exist in CLI but missing from GUI DlssConfig type
2. `tonemapMode` type only has `'none' | 'pq'`, missing `'custom'`
3. Progress parser regex is fragile - Task 5 replaces it
4. GUI uses polling (frame:latest IPC every 3s) instead of event-driven preview - Task 5+9 fix this
5. No camera.json auto-discovery in GUI - Task 7 adds this

### Potential Issues
- When adding `--progress-format json` to buildCliArgs always, ensure `--progress-format` flag is actually implemented in CLI (Task 2) BEFORE Task 5 changes GUI behavior
- `gui/src/ipc/channels.ts` must exist alongside `gui/electron/ipc-channels.ts` - check if it exists
- Need to verify `nlohmann/json` is available in the project headers
