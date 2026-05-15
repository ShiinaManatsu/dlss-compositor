# Development Guide

This document covers the local development workflow for DLSS Compositor — the C++ CLI, the Electron GUI, and the Blender extension.

For end-user instructions see [`build_guide.md`](build_guide.md), [`gui_guide.md`](gui_guide.md), and [`blender_export_guide.md`](blender_export_guide.md).

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Visual Studio 2022 | 17.x | "Desktop development with C++" workload |
| CMake | 3.20+ | Bundled with VS 2022 or installed separately |
| Vulkan SDK | 1.3+ | Sets `VULKAN_SDK` env var; must include `Bin/glslc.exe` |
| Node.js | 18+ LTS | For the Electron GUI |
| Blender | 4.2+ LTS | For testing the extension |
| Git | any | Submodules required (DLSS SDK) |
| FFmpeg | any recent | On `PATH`; needed for GUI live preview and `--encode-video` |

After cloning, fetch the DLSS SDK submodule:

```powershell
git submodule update --init --recursive
```

CMake will fail fast (`FATAL_ERROR`) if `DLSS/include/` is missing.

## Building the C++ CLI

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release --parallel
```

Outputs land in `build/Release/`:
- `dlss-compositor.exe`
- `nvngx_dlss.dll`, `nvngx_dlssg.dll` (auto-copied from `DLSS/lib/Windows_x86_64/rel/`)
- `shaders/*.spv` (compiled by `glslc` from `src/shaders/*.comp`)
- `luts/*.bin` (only if `tools/generate_pq_lut.py` was run)

Debug builds (`--config Debug`) link `nvsdk_ngx_d_dbg.lib` instead of `nvsdk_ngx_d.lib`.

Smoke test the build:

```powershell
build\Release\dlss-compositor.exe --test-vulkan
build\Release\dlss-compositor.exe --test-ngx
build\Release\dlss-compositor.exe --help
```

## Building / Running the GUI

```powershell
cd gui
npm install        # first time only
npm run dev        # dev mode with hot reload
npm run build      # production build → dist/ + dist-electron/
npm run typecheck  # tsc --noEmit
```

`npm run dev` starts `electron-vite dev`, which launches Electron pointing at the Vite dev server. The GUI auto-detects `dlss-compositor.exe` in `build/Release/` relative to the project root — make sure the C++ build is up to date before launching.

For a packaged smoke run after `npm run build`:

```powershell
.\dist-electron\dlss-compositor-gui.exe
```

## VSCode F5 Workflow

`.vscode/launch.json` provides three configurations:

1. **Launch GUI (Dev)** — runs `npm run dev` in `gui/` and attaches to Electron's main process. Hot reload works in the renderer.
2. **Launch GUI (Packaged)** — invokes `npm run build` then launches `dist-electron/dlss-compositor-gui.exe`. Use to validate the production bundle.
3. **Debug C++ CLI** — builds the CMake `Release` target and attaches the MSVC debugger to `dlss-compositor.exe`. Edit the configuration's `args` field to set `--input-dir`, `--output-dir`, etc. for the run you want to debug.

Open the Run and Debug panel (`Ctrl+Shift+D`), choose the configuration, press `F5`.

## Test Commands

```powershell
# C++ unit tests (Catch2)
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# Single C++ test by name
build\Release\tests\dlss-compositor-tests.exe "[motion_vectors]"

# GUI E2E tests (Playwright)
cd gui
npx playwright test                 # all specs
npx playwright test resolution.spec # single spec
npx playwright test --headed        # show the Electron window during runs
```

Playwright is configured for serial execution only (`fullyParallel: false`, `workers: 1`) because Electron cannot parallelize browser instances. Tests that need fixture sequences read from `gui/tests/fixtures/`.

## Project Structure

```
.
├── src/                      # C++ batch processor
│   ├── cli/cli_parser.cpp    # Flag parsing — add new flags here first
│   ├── core/                 # version.h, ndjson_emitter, channel_map, sequence
│   ├── dlss/                 # NGX wrappers (SR, FG)
│   ├── gpu/                  # Vulkan context (compute-only), VMA
│   ├── pipeline/             # sequence_processor.cpp (~2000 lines, three pipelines)
│   └── shaders/              # *.comp → *.spv at build time via glslc
├── gui/
│   ├── electron/             # Main process, IPC handlers, NDJSON parser
│   ├── src/                  # React renderer
│   │   ├── components/       # CommonSettings, SRSettings, FGSettings, AdvancedNgxPanel,
│   │   │                     #   ResolutionPanel, InputOutputPanel, ProcessingView,
│   │   │                     #   StatusBar, Layout
│   │   ├── state/            # Context+useReducer stores (config, processing, banner)
│   │   ├── hooks/            # useProcessing
│   │   ├── types/            # dlss-config.ts (DEFAULT_CONFIG), window.d.ts
│   │   └── ipc/              # Channel name constants (mirror of electron/ipc-channels.ts)
│   ├── tests/                # Playwright specs + fixtures
│   ├── dlss-compositor.json          # User-local settings (gitignored if you choose)
│   └── dlss-compositor.example.json  # Committed template
├── blender/dlss_compositor_aov/  # Blender extension (Python)
├── tests/                    # C++ Catch2 tests + EXR fixtures
├── tools/                    # Python: generate_pq_lut.py, exr_validator.py, ab_compare.py
├── docs/                     # User + dev guides (this file)
├── scripts/package-release.ps1   # Release zip builder
├── DLSS/                     # NVIDIA DLSS SDK submodule (310.5.3)
└── CMakeLists.txt            # Root build
```

## Settings File

The GUI persists configuration to `gui/dlss-compositor.json` (project-relative — NOT Electron's per-user `userData` directory). Two paths matter:

- `gui/dlss-compositor.json` — the active config. Hand-editable JSON; missing keys fall back to `DEFAULT_CONFIG` from `src/types/dlss-config.ts`.
- `gui/dlss-compositor.example.json` — a committed template with every default value. Copy it into `dlss-compositor.json` to reset.

When adding a new field to `DlssConfig`, update three places in lockstep:
1. `src/types/dlss-config.ts` — extend the interface and add a default.
2. `gui/dlss-compositor.example.json` — add the new key with the documented default value.
3. `electron/cli-args.ts` — map the new field to its CLI flag in `buildCliArgs`.

Malformed JSON at startup is caught and replaced with defaults; a warning surfaces in the StatusBar.

## Adding New CLI Flags

Adding a flag end-to-end touches four files. Follow this order to keep the GUI ↔ CLI contract consistent:

1. **`src/cli/cli_parser.cpp`** — add the flag string, parse it, validate range, store on the config struct.
2. **`src/core/config.h`** (or whichever header backs the parser) — add the field with a default that matches the CLI default.
3. **`gui/src/components/AdvancedNgxPanel.tsx`** (or the appropriate settings component) — add the input control, wire it through the config store reducer, and add a default in `DEFAULT_CONFIG`.
4. **`gui/electron/cli-args.ts`** — extend `buildCliArgs` to translate the new field into a CLI argument. Skip emission when the value matches the default to keep the CLI line short.

After editing, update:
- `docs/cli_reference.md` — flag table.
- `docs/gui_guide.md` — relevant settings section if user-visible.
- A Playwright spec in `gui/tests/` if the field has UI behaviour worth pinning.

## Playwright Tests

The GUI E2E suite lives in `gui/tests/` (11 specs). Configuration:
- `gui/playwright.config.ts` — `fullyParallel: false`, `workers: 1`, 30 s timeout.
- Fixtures: `gui/tests/fixtures/with_camera_json/` (3 EXR stubs + camera.json) and `without_camera_json/` (2 EXR stubs).

Common commands:

```powershell
cd gui

# Run everything
npx playwright test

# Run a single spec file
npx playwright test resolution.spec.ts

# Run a single test by title substring
npx playwright test -g "auto-detects source resolution"

# Headed mode (see the Electron window)
npx playwright test --headed

# Debug a failing spec
npx playwright test resolution.spec.ts --debug
```

The renderer can never import `fs`, `path`, or `child_process` directly — all Node access is brokered through the preload bridge (`window.dlssApi`). When writing tests that need filesystem state, prepare fixtures on disk before the spec runs and assert via the IPC surface.

For NDJSON-related tests, the protocol is documented in [`docs/ndjson_protocol.md`](ndjson_protocol.md).

## Release Packaging

```powershell
.\scripts\package-release.ps1                    # version derived from git tag or commit SHA
.\scripts\package-release.ps1 -SkipGuiBuild      # if gui/dist is already current
.\scripts\package-release.ps1 -Version "0.3.0"   # override version
```

Outputs to `dist/`:
- `dlss-compositor-v<VERSION>.zip` — CLI + GUI + DLLs + shaders + LUTs
- `dlss-compositor-blender-v<VERSION>.zip` — Blender extension only
