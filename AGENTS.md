# AGENTS.md — DLSS Compositor v2

This file documents conventions for AI agents working in this codebase.

## Codebase Overview

- `src/`: C++ engine (dlss_engine_lib static + dlss-compositor-cli.exe)
- `gui/`: Flutter Windows GUI
- `blender/`: Blender 5.1+ extension (Python)
- `docs/dlss-refs/`: DLSS SDK doc mirrors (must reference for any NGX work)
- `DLSS/`: NVIDIA DLSS SDK v310.6.0 submodule

## Critical Conventions

### 1. DLSS §-citation Comments (MANDATORY)
Every `NVSDK_NGX_*` API call MUST have a `// DLSS §X.X` reference comment on the same
or preceding line. This prevents UE cargo-cult bug re-introduction. The F1 reviewer
will `grep` for violations and REJECT if any are missing.
Example: `p.InMVScaleX = 1.0f; // DLSS SR §3.6.3 - pixel-unit MVs`

### 2. stdout = RPC Only
stdout is exclusively reserved for JSON-RPC Content-Length framed protocol.
All diagnostic output MUST use `spdlog` → stderr + rotating file sink.
`std::cout`, `printf`, `fprintf(stdout, ...)` are forbidden in src/.

### 3. No Per-Frame vkCreateImage
All GPU texture allocation MUST go through `TexturePool::acquire/release`.
Per-frame `vkCreateImage` is a performance regression from v1.

### 4. Jitter Base=8 (Phase Count)
"base=8" refers to the MINIMUM phase count (DLAA/UQ = 8 phases), NOT the Halton
radix base. Halton uses radix 2 for X, radix 3 for Y. Phase counts per §3.7.1.1:
DLAA=8, UQ=8, Q=18, B=24, P=32, UP=72.

### 5. MV Mapping: DLSS_MV = (Blender.X, -Blender.Y)
Empirically verified in `blender/tests/MV_SANITY_RESULT.md`.
DO NOT use `-blenderMv` (double negation = v1 bug masked by InMVScale error).
X passes through directly (two sign flips cancel). Only Y is negated.

### 6. InReset = First Frame / Gap / Camera Cut Only
`InReset = true` ONLY on: first frame, frame gap in sequence, or camera discontinuity.
NEVER set `InReset = true` every frame — this destroys temporal history.

### 7. SR Before Tonemap, Tonemap Before FG
Per §3.1: SR input must be scene-linear (no EOTF applied yet).
Per FG §HDR: FG backbuffer must be display-ready (after tonemap + EOTF).

### 8. No dnxhr/h264/h265/av1/avif/jpeg/webp in EncoderRegistry
These are enum placeholders only. Do NOT register them in EncoderRegistry.
Do NOT list them in RPC `getStatus().video_encoders` or `export_formats_*`.
See "扩展 Codec 多维政策表" in `.omo/plans/dlss-compositor-v2.md`.

### 9. Schema Single Source of Truth
`src/core/dlssconfig.schema.json` is the SINGLE source for all three layers.
Do NOT copy it to `blender/` or `gui/assets/` — reference it via relative path.

### 10. Agent QA Scenarios
Every task implementation MUST include:
- At least 1 happy path scenario
- At least 1 failure/edge case scenario
- Evidence saved to `.omo/evidence/task-{N}-*.{ext}`

## Build System

```bash
# Configure + build
cmake -B build --preset x64-windows-msvc-release -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release

# Run C++ tests (GPU tests tagged [gpu], skip in CI without NVIDIA)
ctest --test-dir build --config Release --output-on-failure

# Flutter
cd gui && flutter pub get && flutter analyze && flutter test

# Blender pytest (local only, not in CI)
python -m pytest blender/tests/
```

## False Claims to Never Repeat

- "FG clamps HDR to 10 nits" — FALSE. v2 uses ACES Filmic + PQ ST.2084, full range.
- "Supports DX12 output" — NOT supported in v2. Vulkan only.
- "Electron GUI" — NOT true. GUI is Flutter on Windows.
- "NIS upscaling" — NOT present. SR is DLSS only.
- "DNxHR/H264/H265 export" — enum placeholders only, not registered in v2.
