# DLSS Compositor — Knowledge Base

**Generated:** 2026-05-09
**Commit:** 13a1b50
**Branch:** opus-rework

## OVERVIEW

Offline batch processor that feeds Blender Cycles EXR renders through NVIDIA DLSS Super Resolution (SR) and DLSS Frame Generation (FG) via Vulkan + NGX direct API. Three-part toolchain: C++ CLI (`dlss-compositor.exe`), Electron GUI (`dlss-compositor-gui.exe`), Blender 4.2+ extension for pass export and Halton jitter. Windows 10/11 only, RTX Turing+ required (RTX 40+ for FG).

## STRUCTURE

```
.
├── src/              # C++ batch processor (cli/core/dlss/gpu/pipeline/shaders)
├── gui/              # Electron + React GUI, spawns C++ exe as child
├── blender/          # Blender extension — pass config + jitter + camera.json export
├── tests/            # Catch2 C++ tests + fixtures (sequence/, camera.json, static_camera.json)
├── tools/            # Python helpers: LUT gen, exr_validator, ab_compare
├── docs/             # User guides (build, usage, GUI, LUT, CLI reference)
├── scripts/          # package-release.ps1 (release zip builder)
├── assets/           # .gitignored bin files (pre-computed LUTs)
├── DLSS/             # git submodule: NVIDIA DLSS SDK 310.5.3
├── .sisyphus/        # plans (5 phase plans, all landed), evidence, notepads
└── CMakeLists.txt    # Root build: FetchContent + DLSS SDK + glslc shader pipeline
```

## WHERE TO LOOK

| Task | Location |
|------|----------|
| C++ entry point | `src/main.cpp` (135 lines: test modes + sequence processing dispatch) |
| CLI flag parsing | `src/cli/cli_parser.cpp` (503 lines, ~25 flags) |
| Pipeline orchestration | `src/pipeline/sequence_processor.cpp` (1949 lines — largest file; SR / FG / SR+FG paths) |
| NGX SR/FG integration | `src/dlss/ngx_wrapper.cpp`, `src/dlss/dlss_sr_processor.cpp`, `src/dlss/dlss_fg_processor.cpp` |
| Vulkan device init | `src/gpu/vulkan_context.cpp` (497 lines; compute-only, no swapchain — C++ GUI was removed Phase 0) |
| Compute shaders | `src/shaders/*.comp` → SPIR-V at build time (pq_transfer, apply_lut, cas_sharpen) |
| Blender pass setup | `blender/dlss_compositor_aov/__init__.py` (464 lines; Halton jitter, AOV register, camera JSON export) |
| Electron main | `gui/electron/main.ts` (IPC + child process) |
| GUI state | `gui/src/state/config-store.ts` (Context + useReducer) |
| NDJSON emitter | `src/core/ndjson_emitter.cpp` (machine-readable progress for GUI) |
| Version constants | `src/core/version.h` (appver:: namespace — single source) |
| Phase plans | `.sisyphus/plans/phase-{0..4}-*.md` (all landed; reference for design rationale) |
| DLSS SDK headers | `DLSS/include/nvsdk_ngx*.h` (read-only reference; `nvsdk_ngx_helpers_vk.h:89-90` marks GBuffer "research only") |

## BUILD FLOW

Three independent artifacts. Order doesn't matter except at release packaging.

### C++ CLI (Visual Studio 2022 + Vulkan SDK 1.3+ + DLSS submodule)

```powershell
# Prerequisites: VS2022, CMake 3.20+, Vulkan SDK 1.3+ (sets VULKAN_SDK env var)
git submodule update --init --recursive   # pulls DLSS/ (310.5.3)

cmake -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release --parallel

# Output: build/Release/dlss-compositor.exe
#         + nvngx_dlss.dll, nvngx_dlssg.dll (auto-copied from DLSS/lib/Windows_x86_64/rel/)
#         + shaders/*.spv (glslc-compiled compute shaders)
#         + luts/*.bin (if tools/generate_pq_lut.py was run)
```

**Key CMake behavior** (`CMakeLists.txt`):
- `DLSS_SDK_ROOT` is **mandatory** — FATAL_ERROR if unset
- `FetchContent` for OpenEXR, Catch2, volk, VMA, nlohmann_json
- Shaders compiled via `glslc` (searched in `$VULKAN_SDK/Bin`), embedded as .spv files
- Debug build links `nvsdk_ngx_d_dbg.lib`; Release links `nvsdk_ngx_d.lib`
- DLLs copied post-build via `add_custom_command`

### Electron GUI (Node 18+ LTS)

```powershell
cd gui
npm install                  # first time only (Electron 28.3.3, React 18, Tailwind)
npm run dev                  # dev mode: hot reload
npm run build                # production: dist/ + dist-electron/
npx playwright test          # E2E tests (serial; fullyParallel: false)
```

### Blender Extension (no build)

Ship as folder or zip; users install via Blender 4.2+ Preferences → Extensions → Install from Disk:
```
blender/dlss_compositor_aov/   # install this folder or its zip
```

### Release Packaging

```powershell
.\scripts\package-release.ps1                    # auto-versioned from git tag or commit SHA
.\scripts\package-release.ps1 -SkipGuiBuild      # if gui/dist already current
.\scripts\package-release.ps1 -Version "0.3.0"   # override version

# Outputs:
#   dist/dlss-compositor-v<VERSION>.zip          (CLI+GUI+DLLs+shaders+luts)
#   dist/dlss-compositor-blender-v<VERSION>.zip  (Blender extension)
```

### CI (`.github/workflows/build-release.yml`)

Push to `main` or tag `v*` → checkout with submodules → install Vulkan SDK 1.4.309 → cmake configure + build Release → `ctest` → zip artifact → upload; on tag, create GitHub Release.

## RUNTIME REQUIREMENTS

- **GPU**: NVIDIA RTX (Turing+) for SR; RTX 40+ for FG. Driver 535.104+
- **FFmpeg on PATH**: Optional — only needed for GUI live frame preview (EXR→PNG). CLI video encoding (`--encode-video`) also uses FFmpeg
- **No RTX detected** → `--test-ngx` exits with clear error; no silent fallback

## CLI — TOP FLAGS

Full reference: `docs/cli_reference.md`.

```
# I/O
--input-dir <path>         --output-dir <path>
--camera-data <file>       # auto-discovers <input-dir>/camera.json if omitted
--channel-map <file>

# SR
--scale <1.0-8.0>          --quality DLAA|MaxQuality|Balanced|Performance|UltraPerformance
--preset auto|J|K|L|M      # auto uses NVIDIA recommended per-quality preset
--source-fps <N>           # default 30; derives InFrameTimeDeltaInMsec
--sharpness <0.0-1.0>      # post-DLSS CAS sharpen; 0 = off

# FG
--interpolate 2x|4x        # needs --camera-data

# HDR transport (FG only)
--tonemap pq|none|custom   --tonemap-lut <bin>    --inverse-tonemap-lut <bin>
--no-inverse-tonemap

# Output encoding
--encode-video <file.mp4>  --fps <N>

# Diagnostics
--test-ngx                 --test-vulkan
--progress-format human|json   # json = NDJSON for GUI / scripts
--ngx-log-level off|error|warn|info|verbose   # default warn

# Performance
--memory-budget <GB>       # default 8, min 1
```

## CONVENTIONS

Deviations only. See subdir AGENTS.md for stack-specific rules.

- **C++**: No namespaces; `PascalCase` classes with `m_` member prefix; `camelCase` methods; `snake_case` filenames. See [`src/AGENTS.md`](src/AGENTS.md).
- **TypeScript/React**: Components default-export `PascalCase`; hooks `camelCase` with `use` prefix. See [`gui/AGENTS.md`](gui/AGENTS.md).
- **Python (Blender)**: `snake_case` + module-global `_runtime_jitter` dict for handler state. See [`blender/AGENTS.md`](blender/AGENTS.md).
- **Version constants**: Always from `src/core/version.h` `appver::` namespace — never redefine locally.
- **NGX API path**: Direct NGX (`NVSDK_NGX_VULKAN_*`), **not** Streamline. GBuffer feeding intentionally **not used** for SR (per NVIDIA header "research only" marker).

## ANTI-PATTERNS

- ❌ **Don't populate `evalParams.GBufferSurface`** for DLSS-SR — verified wasted (Phase 2 deleted entire path). Only DLSS-RR uses GBuffer as first-class params.
- ❌ **Don't hardcode `InFrameTimeDeltaInMsec = 33.3f`** — derive from `--source-fps` via `1000/fps`.
- ❌ **Don't re-introduce C++ ImGui GUI code** — Electron is the only GUI (Phase 0 removed ~1370 LOC). Vulkan context is compute-only; no swapchain/surface/GLFW.
- ❌ **Don't compute Halton jitter outside `blender/dlss_compositor_aov/__init__.py` `apply_jitter()` handler** — render handler stores values in `_runtime_jitter` dict; `export_camera` reads from dict (single source of truth, Phase 1 task 5).
- ❌ **Don't duplicate version constants in `.cpp` files** — consolidated in `src/core/version.h` (Phase 1 task 1).
- ❌ **Don't emit human-readable progress on stdout when `--progress-format json`** — reserved for NDJSON events. Logs go to stderr.

## COMMANDS

```powershell
# Most common
cmake --build build --config Release
ctest --test-dir build --config Release --output-on-failure
cd gui; npm run dev
.\scripts\package-release.ps1

# Smoke tests
build\Release\dlss-compositor.exe --test-ngx
build\Release\dlss-compositor.exe --test-vulkan
build\Release\dlss-compositor.exe --help
```

## NOTES & GOTCHAS

- **DLSS SDK is a submodule**, not FetchContent — `git submodule update --init --recursive` is required after fresh clone. CMake fails fast if `DLSS/include/` missing.
- **Shaders compile at build time** via `glslc`; if Vulkan SDK's `Bin/glslc.exe` is missing, build fails. Install Vulkan SDK with "Shader Compiler" component.
- **DLL runtime version != compile-time version**: NVIDIA driver may inject newer `nvngx_dlss.dll`. Actual DLL version is queried at runtime (`getDllFileVersion` in `ngx_wrapper.cpp`) and printed in NGX banner.
- **First frame quality is lower**: DLSS-SR is temporal; `InReset=true` on frame 0 resets history. Acceptable by design (offline batch, not live).
- **Cycles denoising should be ON upstream**: DLSS-SR is trained on clean game renders; path-traced noise degrades output. Use Cycles OIDN before export.
- **Jitter phases scale with output**: `8 × output_scale²` per NVIDIA spec (Phase 1 task 6). Stored in camera.json as `jitter_phases`. 2x upscale → 32 phases (not hardcoded 8).
- **sequence_processor.cpp is ~2000 lines** — three pipelines (SR / FG / SR+FG) with some duplication. Refactor into shared helpers is Phase 4 follow-up but not done yet.
- **NGX banner** is printed once at startup (structured `[NGX] ...` lines). GUI regex-parses to populate StatusBar.
- **FG uses PQ tonemap by default** for HDR transport (DLSS-FG internally clamps ~10 nits). Disable with `--tonemap none` only if source has no highlights above 1.0.
- **`.sisyphus/` holds all planning history** — 5 phase plans (all landed), Momus reviews, evidence files. Reference before proposing design changes.
