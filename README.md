# DLSS Compositor

> Offline Blender Cycles renders through NVIDIA DLSS — Super Resolution (SR), Ray Reconstruction (RR) and Frame Generation (FG) — driven by an Electron GUI, producing ProRes / EXR / PNG sequences.

## What It Does

The GUI (or CLI) drives a three-way pipeline: a Blender session injects camera jitter and renders multilayer EXR input buffers, a resident C++/Vulkan CLI consumes them through the DLSS engine (SR / RR → tonemap → FG), and results stream out as fragmented ProRes MOV (crash-safe), scene-linear EXR, or 16-bit PNG. Input frames flow over an ImDisk RAM disk (watch + consume + delete) or any user-chosen cache directory. Projects save as `.dcp` (JSON).

## Repo Layout

- `cli/` - native Vulkan/NGX engine (`dlss-compositor-cli`) and shaders
- `app/` - Electron + React Spectrum 2 GUI; electron-builder packaging (`npm run dist`)
- `pipeline/` - Blender-side inject/inspect scripts, RAM-disk helper, RPC client
- `blender-plugin/` - Blender extension source (LUT generator)
- `tools/` - DLL version pins and MSVC rebuild helpers
- `docs/` - guides, bundled DLSS refs, research notes
- `test-frames/` - retained render sample metadata (frames themselves are not committed)

**Pipeline:**

```
Blender Cycles (inject.py: jitter + passes + dlssconfig.json)
  → RAM disk / cache dir (multilayer EXR)
  → dlss-compositor-cli --rpc-stdio
      EXR prefetch (wait-for-files) → channel map → DLSS SR / RR
      → tonemap + EOTF → DLSS FG → inverse tonemap
      → fragmented ProRes MOV / EXR / PNG16
```

## Requirements

- **GPU**: NVIDIA RTX (Turing+ for SR; Ada/Blackwell for FG/RR)
- **OS**: Windows 10/11 (64-bit)
- **Driver**: NVIDIA 535+ (provides the NGX core; feature DLLs ship with the app)
- **RAM disk**: ImDisk Toolkit (optional; needs an elevated token — otherwise use a cache directory)
- **Build**: Visual Studio 2022+, CMake 3.25+, vcpkg, Node.js + npm
- **Blender**: 5.1+ (SR), 5.3+ (RR guide passes)

## Quick Start

1. **Clone**: `git clone --recurse-submodules <repo-url> && cd dlss-compositor`
2. **Setup vcpkg**: `git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.bat && set VCPKG_ROOT=<path>`
3. **Build CLI**: `tools\_rebuild_cli.bat` (or `cmake --preset x64-windows-msvc-release && cmake --build --preset x64-windows-msvc-release`)
4. **Run GUI (dev)**: `cd app && npm install && npm run dev`
5. **Package**: `cd app && npm run dist` → NSIS installer (`.dcp` association) + portable zip under `app/release/`
6. **Blender extension**: Preferences → Extensions → Install from Disk → `blender-plugin/dlss_compositor/`

For local use, `cd app && npm run dist:local-nr` builds one Windows x64 `DLSS-Compositor-Setup-<version>-NR.exe` under `build/release/`. This installer embeds the existing `resources/nvngx_dlssnr.dll` and installs it beside the CLI. The standard `npm run dist` artifacts still exclude that DLL.

## Supported Output Formats

| Mode | Formats |
|------|---------|
| LDR | ProRes 422 HQ, PNG (16-bit), EXR (scene-linear) |
| HDR | ProRes 422 HQ, EXR |

## Features

- DLSS SR upscaling with per-quality presets; DLAA at 100%
- DLSS RR (Cycles, quality slots, preset F) with full guide-buffer mapping
- DLSS FG 2x–6x frame interpolation on SR or RR output
- NR/FG tool with compact FFV1 guide videos (`dlssnrbuffer.mkv`) and legacy EXR input; NR model A/B selection with a local NR 310.9.16 runtime
- Watch-mode RAM-disk pipeline with prefetch, backpressure, and stop/cancel that always leaves a playable file
- Rolling parallel Blender rendering sizes windows from uncompressed EXR buffers and keeps one continuous DLSS history across the export
- `.dcp` projects with autosave, job history, and file association
- Packaged builds auto-create/unmount the DLSSRAM disk (elevated runs)
- JSON-RPC stdio interface for automation

## DLSS Technical Notes

- **SR input**: scene-linear HDR before any tonemap (DLSS SR §3.1)
- **FG input**: display-ready (after tonemap + EOTF) (DLSS FG §HDR)
- **RR sampling**: TABULATED_SOBOL + animated seed (DLSS-RR §3.5)
- **Motion vectors**: pixel-space, curr→prev, `InMVScale = (1,1)` (§3.6.3)
- **MV mapping**: `DLSS_MV = (Blender.X, -Blender.Y)` (empirically verified)
- **InReset**: first frame / frame gap / camera discontinuity only
- **SDK and feature DLLs**: SDK v310.9.16; nvngx_dlss / nvngx_dlssd / nvngx_dlssg 310.9.16.0 from the SDK's Windows x64 release bundle, pinned in `resources/` and loaded from next to the exe; NGX core always from the driver

## Documentation

- [Build Guide](docs/build_guide.md)
- [Blender Export Guide](docs/blender_export_guide.md)
- [CLI Reference](docs/cli_reference.md)
- [User Manual (bilingual HTML)](docs/user_manual.html)

## License

MIT — see [LICENSE](LICENSE)
