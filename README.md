# DLSS Compositor v2

> First open-source tool for processing offline Blender Cycles renders through NVIDIA DLSS Super Resolution (SR) and Frame Generation (FG) — AI-powered upscaling and frame interpolation for animation sequences.

## What It Does

DLSS Compositor v2 reads multi-channel EXR sequences from Blender Cycles (Combined RGBA + Depth + Motion Vector passes), applies NVIDIA DLSS SR for upscaling and/or DLSS FG for frame interpolation, and outputs scene-linear EXR or 16-bit PNG image sequences.

**Three-layer architecture:**
- **Blender 5.1+ Extension** — Quality mode selection, render pass configuration, transparent jitter injection, silent `dlssconfig.json` export
- **Flutter Windows GUI** — Task configuration, real-time preview frames (zero-copy FFI), progress monitoring
- **C++ Vulkan/NGX Engine** — Offline SR+FG pipeline via NVIDIA NGX direct (no Streamline)

## Pipeline Overview

```
Blender Cycles render
  → dlssconfig.json (per-frame camera + jitter + config)
  → DLSS Compositor v2 engine
  → [Stage 1] EXR prefetch + channel map + depth/MV shaders
  → [Stage 2] DLSS SR (scene-linear render-res → output-res)
  → [Stage 4] Tonemap + EOTF (for FG path only)
  → [Stage 5] DLSS FG (display-ready → interpolated frames)
  → [Stage 6] Inverse Tonemap (for FG path only, EXR output)
  → [Stage 7] Async write (EXR/PNG)
```

## Requirements

- **GPU**: NVIDIA RTX (Turing+ for SR; Ada/Blackwell for FG)
- **OS**: Windows 10/11 (64-bit)
- **Driver**: NVIDIA 535+
- **Build Tools**: Visual Studio 2022, CMake 3.20+, Vulkan SDK 1.3+, vcpkg
- **Flutter**: 3.22+ (for GUI)
- **Blender**: 5.1+ (for extension)

## Quick Start

1. **Clone**: `git clone --recurse-submodules <repo-url> && cd dlss-compositor`
2. **Setup vcpkg**: `git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.bat && set VCPKG_ROOT=<path>`
3. **Build C++**: `cmake -B build --preset x64-windows-msvc-release -DDLSS_SDK_ROOT=DLSS && cmake --build build --config Release`
4. **Build GUI**: `cd gui && flutter pub get && flutter build windows --release`
5. **Install Blender extension**: Blender → Edit → Preferences → Extensions → Install from Disk → select `blender/dlss_compositor/`
6. **Render**: Open Render Properties → DLSS Compositor → select quality → Apply Settings → render animation
7. **Process**: Launch `dlss_compositor_gui.exe` → Open `dlssconfig.json` → configure → Start

## Supported Output Formats

| Mode | Formats |
|------|---------|
| LDR project | PNG (16-bit), EXR (scene-linear) |
| HDR project | EXR (scene-linear) |

## Features

- DLSS SR 1x–3x upscaling (DLAA/Quality/Balanced/Performance/UltraPerformance)
- DLSS FG 2x/3x/4x frame interpolation (RTX 40+ required)
- Combined SR+FG pipeline with zero-copy GPU texture sharing
- Halton(2,3) jitter sequence (base=8, per-quality phase counts per DLSS §3.7.1.1)
- Per-quality NGX presets (DLAA/Q/B/UQ→K, P→M, UP→L per §3.12)
- Scene-linear HDR pipeline (no internal clamp — full dynamic range preserved)
- ACES Filmic + PQ ST.2084 tonemap for FG HDR path
- Async EXR/PNG writer with async frame prefetcher
- Flutter GUI with live preview (FFI zero-copy shared memory)
- JSON-RPC stdio fallback for automation

## DLSS Technical Notes

- **SR input**: scene-linear HDR before any tonemap (DLSS SR §3.1)
- **FG input**: display-ready (after tonemap + EOTF) (DLSS FG §HDR)
- **Motion vectors**: pixel-space, curr→prev, `InMVScale = (1,1)` (§3.6.3)
- **MV mapping**: `DLSS_MV = (Blender.X, -Blender.Y)` (empirically verified)
- **InReset**: first frame / frame gap / camera discontinuity only (not every frame)
- **DLSS SDK**: v310.6.0

## Documentation

- [Build Guide](docs/build_guide.md)
- [Blender Export Guide](docs/blender_export_guide.md)
- [GUI Guide](docs/gui_guide.md)
- [CLI Reference](docs/cli_reference.md)
- [User Manual (bilingual HTML)](docs/user_manual.html)

## License

MIT — see [LICENSE](LICENSE)
