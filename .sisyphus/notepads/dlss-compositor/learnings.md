# Learnings — dlss-compositor

## Project Overview
- C++ application for offline DLSS Ray Reconstruction denoising of Blender Cycles render sequences
- Platform: Windows 10/11, MSVC only
- Key libraries: Vulkan (raw, no nvrhi), NGX DLSS-RR, tinyexr, ImGui, volk, VMA, Catch2
- DLSS SDK provided as git submodule at DLSS/ (official NVIDIA repo)

## Architecture
- src/core/ — CPU-side: EXR reading, channel mapping, motion vector conversion
- src/gpu/ — Vulkan bootstrap and texture pipeline
- src/dlss/ — NGX wrapper and DLSS-RR processor
- src/pipeline/ — Sequence processor
- src/ui/ — ImGui viewer
- src/cli/ — CLI argument parser
- blender/ — Blender add-on script
- tools/ — Python EXR validator
- tests/ — Catch2 unit tests + pytest
- tests/fixtures/ — Reference 64x64 EXR files

## Key Decisions
- DLSS-RR only (no OIDN fallback) — project differentiator
- Animation sequences only (temporal denoising, single frame quality poor)
- Input resolution < output resolution (DLSS upscales)
- Jitter = 0 for V1
- Non-RTX = error and exit (no fallback)
- No Donut framework — raw Vulkan + volk/VMA + ImGui only
- tinyexr (single-header) not OpenEXR for C++ 
- DLSS Feature ID: NVSDK_NGX_Feature_RayReconstruction (14), NOT SuperSampling
- Use NGX_VULKAN_CREATE_DLSSD_EXT1 (note "1" suffix and "D" for Denoiser)

## CMake Notes
- DLSS SDK path via DLSS_SDK_ROOT variable (not fetched by CMake)
- tinyexr + Catch2 + ImGui + volk + VMA via FetchContent or submodules
- C++17, MSVC
- Visual Studio 17 2022 generator
- tinyexr reader/writer needs explicit miniz linkage on Windows builds; linking the core static lib against tinyexr + miniz avoids unresolved mz_* symbols.
- SaveEXR writes reliably for a packed RGBA buffer; round-trip tests should use a generated RGBA image even when validating a single channel.

## Task 3 Learnings
- Channel mapping is CPU-only and should fail fast on missing required Blender channels: Combined RGBA, Depth, and all 4 motion-vector channels.
- Optional DLSS-RR inputs should be materialized with defaults: diffuse white, specular black, normals +Z, roughness 0.5.
- Catch2 fixture tests are safe to skip locally when EXRs are absent; keep default behavior covered by the non-fixture unit test.
