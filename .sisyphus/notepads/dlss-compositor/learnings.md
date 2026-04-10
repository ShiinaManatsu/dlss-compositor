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

## [2026-04-11] Task 9: NGX DLSS-RR Wrapper
- VulkanContext uses NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements to dynamically query NGX-required device extensions (VK_NVX_image_view_handle, VK_NVX_binary_import, VK_KHR_push_descriptor etc.) — no static list needed
- NGX init: use NVSDK_NGX_VULKAN_Init_with_ProjectID, pass vkGetInstanceProcAddr + vkGetDeviceProcAddr from volk
- DLSS-RR availability: check NVSDK_NGX_Parameter_SuperSamplingDenoising_Available (int) via GetCapabilityParameters
- nvngx_dlssd.dll must be present in the exe directory — it was already there from DLSS SDK lib copy step
- GPU arch 0x1B0 = RTX 5070 Ti (Blackwell); snippet requires at least 0x160 (Turing) — compatible
- FeatureInitResult = NvNGXFeatureInitSuccess confirmed on RTX 5070 Ti with driver 595.79
- createDlssRR uses NVSDK_NGX_VULKAN_CreateFeature1 (not the NGX_VULKAN_CREATE_DLSSD_EXT1 macro) — both work but the direct API call is cleaner
- NVSDK_NGX_DLSS_Depth_Type_Linear = 0 (linear depth, not hardware depth buffer)

## [2026-04-11] Task 8: Vulkan Texture Pipeline
- Used VMA staging buffers with host-access sequential write for uploads and host-access random read for downloads, plus explicit flush/invalidate around mapped transfers.
- CPU-side float16 packing/unpacking is handled in TexturePipeline for VK_FORMAT_R16G16_SFLOAT and VK_FORMAT_R16G16B16A16_SFLOAT while R32 formats stay float32 end-to-end.
- Image layout transitions follow UNDEFINED -> TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL for upload and SHADER_READ_ONLY_OPTIMAL -> TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL for download.
- CMake added src/gpu/texture_pipeline.cpp to dlss_compositor_core and tests/test_texture_pipeline.cpp to dlss-compositor-tests.
## [2026-04-11] Task 9: NGX DLSS-RR Wrapper
- Used NGX libs: Release `${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`, Debug `${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib`; runtime DLL copied/used: `DLSS/lib/Windows_x86_64/rel/nvngx_dlssd.dll`
- NGX init worked reliably with `NVSDK_NGX_VULKAN_Init_with_ProjectID` using a GUID-like custom project id and engine type `NVSDK_NGX_ENGINE_TYPE_CUSTOM`; plain app id `0` caused the DLSSD snippet validation to reject init for runtime availability
- DLSS-RR availability check uses NGX capability params, primarily `SuperSamplingDenoising.Available`/`FeatureInitResult`, and succeeds only after enabling NGX-required Vulkan instance/device extensions before Vulkan instance/device creation
- Important Vulkan gotcha: query required NGX Vulkan extensions via `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` and `NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements` for `NVSDK_NGX_Feature_RayReconstruction`; missing these left NGX failing on Vulkan entry points and reporting DLSS-RR unavailable

## [2026-04-11] Task 10: DLSS-RR Evaluation
- `DlssRRProcessor` only performs evaluation; DLSS-RR feature creation stays in `NgxContext::createDlssRR()` via `NVSDK_NGX_VULKAN_CreateFeature1`.
- Single-frame DLSS-RR evaluation succeeded with seven wrapped Vulkan image/view inputs: color, depth, motion vectors, diffuse albedo, specular albedo, normals, roughness, plus a writable output image.
- For this SDK, `NVSDK_NGX_Resource_VK` image-view resources can be populated directly with `Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW`, image/view handles, subresource range, format, dimensions, and `ReadWrite`.
- DLSS-RR feature creation on this machine required both `NVSDK_NGX_DLSS_Feature_Flags_IsHDR` and `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes`; without them NGX creation failed first with `HDR Color required` and then `Low resolution Motion Vectors required`.
- The processor test passes under `ctest --test-dir build -C Release -R test_dlss_rr_processor --output-on-failure`, and `build/Release/dlss-compositor.exe --test-ngx` still reports `DLSS-RR available: true`.
- Local verification could not use `lsp_diagnostics` because `clangd` is not installed in the environment.

## [2026-04-11] Task 11: Sequence Processor
- `SequenceProcessor` streams one EXR frame at a time through read → map → MV convert → upload → evaluate → download → write, then destroys all 8 texture handles before the next frame.
- Sequence ordering uses trailing digits from the filename stem for natural numeric sort; reset flags are raised on the first frame and on the first frame after any numeric gap.
- Reused the Task 10 Vulkan command-buffer pattern twice: once for `createDlssRR()` before the frame loop, then per-frame for DLSS-RR evaluation with explicit output transitions `SHADER_READ_ONLY_OPTIMAL -> GENERAL -> SHADER_READ_ONLY_OPTIMAL`.
- End-to-end verification passed with `ctest -C Release -R test_sequence_processor --output-on-failure` and `build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir test_out/ --scale 2`; outputs were written to `.sisyphus/evidence/task-11-sequence.txt` and `.sisyphus/evidence/task-11-e2e.txt`.

### Task 12: ImGui + Vulkan Viewer Scaffold
- Created App class encapsulating GLFW window, raw Vulkan setup (VkInstance, VkDevice, VkSwapchainKHR), and ImGui context.
- ImGui backend was initialized with NO_PROTOTYPES. This required explicit dynamic loading of Vulkan pointers via ImGui_ImplVulkan_LoadFunctions.
- Implemented a headless --test-gui mode using glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE).

### Task 13: Channel Preview + Before/After Split View
- Implemented ImageViewer class to load, map, and upload EXR channels for UI display.
- ImGui uses ImGui_ImplVulkan_AddTexture to display Vulkan images, requiring VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and a sampler.
- Integrated VMA initialization in the main App and passed a compute VulkanContext and TexturePipeline from main.cpp to the GUI.
- Added --test-load CLI flag to automatically load a file during the automated smoke test.

## [2026-04-11] Task 16: Python EXR Validator
- OpenEXR Python package is available after installing requirements; fixture inspection shows Blender-style channel names prefixed with `RenderLayer.`.
- `reference_64x64.exr` and `sequence/frame_0001.exr` contain Combined RGBA, Depth Z, Vector XYZW, Normal XYZ, DiffCol RGB, GlossCol RGB, and Roughness X.
- `missing_channels_64x64.exr` contains Combined RGBA + Depth Z + Vector XYZW only; it is missing Normal, DiffCol, GlossCol, and Roughness channels.
- The validator matches Blender channel suffixes, and `python -m pytest` is the reliable local test entrypoint when `pytest.exe` is absent from PATH.

 # #   [ 2 0 2 6 - 0 4 - 1 1 ]   T a s k   1 7 :   D o c u m e n t a t i o n 
 -   C r e a t e d   R E A D M E . m d   w i t h   p r o j e c t   o v e r v i e w   a n d   q u i c k   s t a r t . 
 -   C r e a t e d   d o c s / b u i l d _ g u i d e . m d   w i t h   V S 2 0 2 2 / C M a k e   b u i l d   s t e p s . 
 -   C r e a t e d   d o c s / u s a g e _ g u i d e . m d   w i t h   B l e n d e r   a n d   C L I   i n s t r u c t i o n s . 
 -   C r e a t e d   d o c s / d l s s _ i n p u t _ s p e c . m d   w i t h   t e c h n i c a l   d e t a i l s   o n   E X R   c h a n n e l s   a n d   m o t i o n   v e c t o r   c o n v e r s i o n .  
 
