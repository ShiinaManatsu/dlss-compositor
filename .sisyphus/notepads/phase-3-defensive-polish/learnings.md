# Learnings - Phase 3 Defensive Polish

## 2026-05-07 Session Start

### Codebase Patterns

**Tests (C++)**: Use Catch2. Test files go in `tests/`. Include path: `${CMAKE_SOURCE_DIR}/src`. Link to `dlss_compositor_core` and `Catch2::Catch2WithMain`.

**CMakeLists.txt pattern**: Add new `.cpp` file to `add_executable(dlss-compositor-tests ...)` block. Use `catch_discover_tests(dlss-compositor-tests)` for auto-discovery. Named tests use `add_test(NAME ... COMMAND dlss-compositor-tests "[tag]")`.

**Logger**: Use `Log::error()` for stderr output, `Log::info()` for stdout. Both write to log file too. `Log::warn()` does NOT exist — use `Log::error()` for warnings or `fprintf(stderr, ...)`.

**MatrixUtil**: Row-major multiply: `tmp[r][c] += a[r][k] * b[k][c]`. Inverse is Gauss-Jordan. Identity is in-place.

**Camera JSON format**: version (int), render_width, render_height, jitter_phases (optional, default 8), frames (object with 4-digit string keys like "0200"). Per frame: matrix_world (4x4), projection (4x4), fov (float), aspect_ratio (float), near_clip, far_clip, jitter_x (optional), jitter_y (optional).

**computePairParams logic**: `clipToPrevClip = prevProj * prevView * currWorld * currProjInv`. For static camera: currWorld == prevWorld and currProj == prevProj → result is identity.

**Jitter assignment (sequence_processor.cpp:797-811)**: Direct assignment from `camData.jitter_x/y`. No clamp. `Log::info` is used not `Log::warn`.

**LUT format**: `lutSize^3 × 4 × sizeof(float)` bytes. Indexed as X fastest, Z slowest (row-major 3D). Loaded by `lut_processor.cpp:loadLut()`.

**PQ constants (from pq_transfer.comp)**:
- m1 = 2610/16384 = 0.1593017578125
- m2 = 2523/4096*128 = 78.84375
- c1 = 3424/4096 = 0.8359375
- c2 = 2413/4096*32 = 18.8515625
- c3 = 2392/4096*32 = 18.6875
- SDR_WHITE_NITS = 100, PQ_MAX_NITS = 10000
- Scene-linear 1.0 = 100 nits → scale = SDR_WHITE_NITS/PQ_MAX_NITS = 0.01

**Requirements.txt**: Located at root. Currently has `OpenEXR>=3.0.0` and `pytest>=7.0.0`.

**Docs directory**: `docs/` exists with build_guide.md, usage_guide.md, etc.

**Assets directory**: Does NOT exist yet. Need to create `assets/.gitignore`.

**Log::warn does NOT exist** — the logger only has `error()`, `info()`, `vwrite()`. Use `Log::error()` for warnings, with "[jitter]" prefix as the plan says.
