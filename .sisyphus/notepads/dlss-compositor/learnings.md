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

## Task 4 Learnings
- Blender Vector pass channels 0/1 map directly to DLSS-RR motion vectors after negation; channels 2/3 are ignored.
- DLSS scale factors should be derived from render dimensions as 1/width and 1/height.
- Catch2's Approx helper was avoided in favor of std::fabs comparisons for compatibility with the current include setup.

## Task 5 Learnings
- CLI parsing is simplest as a hand-rolled loop with a small helper for optional `--encode-video` filenames.
- Tests in `tests/` need direct relative includes for new `src/cli` headers unless the test target adds that directory explicitly.

## [2026-04-10] Task 6: EXR Fixtures
- Used pure Python stdlib + numpy to write minimal EXR files (no OpenEXR package needed)
- Fixture files are committed to git (*.exr not in .gitignore)
- missing_channels_64x64.exr includes Combined RGBA + Depth Z + Vector XYZW (9 channels � NOT just 5)
- Script also copies fixtures to build/tests/tests/fixtures/ for ctest to find them at relative paths
- CRITICAL BUG FIXED: channel_mapper.cpp was not populating out.depth (depthChannel pointer was checked but never copied into out.depth vector) � fixed by adding out.depth.assign(depthChannel, depthChannel + pixelCount)
- All 24 Catch2 tests now pass with 0 skipped
## [2026-04-10] Task 7: Vulkan Bootstrap
- Use volkInitialize() NOT manual LoadLibraryA � volk manages the loader internally
- GLFW must call glfwVulkanSupported() before volkLoadInstance() to ensure Vulkan is present
- VMA needs VMA_STATIC_VULKAN_FUNCTIONS=0 and VMA_DYNAMIC_VULKAN_FUNCTIONS=1 with explicit VmaVulkanFunctions struct pointing to vkGetInstanceProcAddr/vkGetDeviceProcAddr
- VkDevice extensions: VK_KHR_swapchain + VK_KHR_maintenance1 (warn if missing, don't fail)
- GPU selected by: discrete GPU + NVIDIA vendor ID (0x10DE) or "NVIDIA" in device name
- volk-only linking avoids the access violation seen when also linking Vulkan::Vulkan with volk-managed global entry points

## [2026-04-11] Task 8: Vulkan Texture Pipeline
- Used VMA staging buffers with host-access sequential write for uploads and host-access random read for downloads, plus explicit flush/invalidate around mapped transfers.
- CPU-side float16 packing/unpacking is handled in TexturePipeline for VK_FORMAT_R16G16_SFLOAT and VK_FORMAT_R16G16B16A16_SFLOAT while R32 formats stay float32 end-to-end.
- Image layout transitions follow UNDEFINED -> TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL for upload and SHADER_READ_ONLY_OPTIMAL -> TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL for download.
- CMake added src/gpu/texture_pipeline.cpp to dlss_compositor_core and tests/test_texture_pipeline.cpp to dlss-compositor-tests.
