# Draft: DLSS Frame Generation Integration for dlss-compositor

## Requirements (confirmed)
- User wants frame interpolation (2x and 4x) for offline-rendered Blender EXR sequences
- RIFE was original plan but user notes it can't handle HDR properly
- User suggests using DLSS Frame Generation instead, since the DLSS SDK already includes DLSS-G headers and DLL
- Must preserve scene-linear EXR output (HDR-safe, color-preserving)
- Must support Rec.2100-HLG HDR output path
- "补帧后颜色不变" — interpolated frames must not alter color/tonality of original frames

## Key Discovery: DLSS SDK Already Has Everything Needed

### Headers found in DLSS/include/:
- `nvsdk_ngx_defs_dlssg.h` — DLSS-G definitions, parameter names
- `nvsdk_ngx_helpers_dlssg_vk.h` — Vulkan helpers (NGX_VK_CREATE_DLSSG, NGX_VK_EVALUATE_DLSSG)
- `nvsdk_ngx_params_dlssg.h` — Create params and Eval params structs

### DLL found:
- `DLSS/lib/Windows_x86_64/rel/nvngx_dlssg.dll` — The Frame Generation runtime DLL (already in SDK!)

### DLSS 4 Multi-Frame Support (from headers):
- `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` — Max generated frames supported (3 for 4x)
- `NVSDK_NGX_DLSSG_Parameter_MultiFrameCount` — Number of frames to generate (1=2x, 3=4x)
- `NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex` — Current inner frame index (1..MultiFrameCount)
- This means 4x works by calling evaluate() multiple times with different indices!

### Documentation found:
- `DLSS/doc/DLSS-FG Programming Guide.pdf`

## Technical Analysis: DLSS-G API for Offline Usage

### What DLSS-G Needs (from headers + PDF):

**Required inputs per frame:**
1. **Backbuffer** (final color) — `NVSDK_NGX_Resource_VK*` — We have this from DLSS-RR output (R16G16B16A16_SFLOAT)
2. **Depth** — `NVSDK_NGX_Resource_VK*` — We have this from Blender EXR (R32_SFLOAT)
3. **Motion Vectors** — `NVSDK_NGX_Resource_VK*` — We have this from Blender EXR (R16G16_SFLOAT)
4. **Output Interpolated Frame** — `NVSDK_NGX_Resource_VK*` — We allocate this

**Optional inputs:**
5. HUDless color — Not needed (no HUD in offline renders)
6. UI — Not needed (no UI overlay)
7. OutputRealFrame — Optional
8. OutputDisableInterpolation — Optional (4-byte buffer)

**Required camera parameters (NVSDK_NGX_DLSSG_Opt_Eval_Params):**
- Camera matrices: cameraViewToClip, clipToCameraView, clipToPrevClip, prevClipToClip
- Camera vectors: position, up, right, forward
- Camera properties: near, far, FOV, aspect ratio
- Motion vector scale: mvecScale[2]
- Flags: colorBuffersHDR, depthInverted, cameraMotionIncluded, reset

### Critical Architectural Insight

DLSS-G operates on the GPU-side Vulkan textures directly — it computes **optical flow internally** using NVIDIA hardware (no app-provided OF). This is fundamentally different from RIFE which is a pure image-based interpolator.

This means:
- **No tonemap/inverse-tonemap needed!** DLSS-G works in whatever color space the input is in
- If `colorBuffersHDR=true`, it should handle scene-linear float16 directly
- The "补帧后颜色不变" constraint is naturally satisfied — no color space conversion pipeline
- HDR (Rec.2100-HLG) compatibility comes for free since we're working in scene-linear throughout

### Challenge: Camera Metadata

DLSS-G needs per-frame camera matrices. Blender EXR files **may not contain** camera matrices as metadata.

**Options:**
1. Export camera data from Blender alongside EXR frames (Python script enhancement)
2. Synthesize minimal camera data (identity matrices for static camera, approximate for moving)
3. Use motion vectors to derive clip-to-prev-clip transform

This is the biggest integration challenge — RIFE didn't need camera data, DLSS-G does.

### Challenge: Swapchain / Presentation Dependency

DLSS-G was designed for real-time game rendering. Key concern:
- Does `NGX_VK_EVALUATE_DLSSG` work without a swap chain?
- Can we just submit command buffers with uploaded textures?
- The raw NGX API (not Streamline) seems more promising for headless/offline use

**Evidence for offline feasibility:**
- The VK helpers in `nvsdk_ngx_helpers_dlssg_vk.h` use raw `VkCommandBuffer` — no swapchain dependency visible in the API
- Our existing DLSS-RR integration already works headlessly (no display, just Vulkan compute)
- NGX init doesn't require a surface/swapchain

**Research pending:** Librarian agents investigating this question.

## Technical Decisions
- **DLSS-G over RIFE**: RIFE can't handle HDR; DLSS-G operates natively in any color space
- **Raw NGX API over Streamline**: Streamline is designed for game engine integration with swapchain hooks; raw NGX is simpler for offline

## Research Findings (from librarian agents)

### Offline Feasibility (CONFIRMED ✅)
- **Swapchain NOT required** — Raw NGX API (`NVSDK_NGX_VULKAN_EvaluateFeature`) works on plain VkCommandBuffer without swapchain
- **Evidence**: NVIDIA dev forum post (2023) + dlssg-to-fsr3 (4.9k stars) successfully called DLSS-G without swapchain
- **Streamline is NOT required** — Raw NGX works standalone, but is unsupported/undocumented
- **Key advantage for offline**: No frame pacing needed (SL Pacer irrelevant), no latency concerns
- **Risk**: NVIDIA hasn't officially supported offline use case; API surface is technically undocumented

### Input Format Requirements (CONFIRMED ✅)
- **R16G16B16A16_SFLOAT**: ✅ Supported for scene-linear HDR backbuffer
- **colorBuffersHDR=1**: Tells DLSS-G input is scene-linear float, NOT tone-mapped — perfect for our EXR pipeline
- **Motion vectors**: Screen-space pixel units in RG float16, with `mvecScale = [1/W, 1/H]` for normalization
- **Camera matrices**: Right-handed column-major (matches Vulkan/OpenGL conventions). Blender is RH too — direct compatibility
- **All matrices must be UNJITTERED** — no TAA jitter offset in matrices (jitter passed separately)
- **Depth**: Linear depth with `depthInverted` flag to control linearization formula

### Hardware Requirements (CONFIRMED)
- **RTX 30-series**: ❌ NO Frame Generation support at all
- **RTX 40-series**: ✅ Basic FG (2x) — uses hardware Optical Flow Accelerator (OFA)
- **RTX 50-series**: ✅ Multi-Frame Gen (up to 4x) — uses AI-based optical flow model
- **Minimum driver**: 550+ for Vulkan DLSS-G

### Multi-Frame 4x Calling Convention (CONFIRMED)
- For 4x: Set `multiFrameCount=3`, call evaluate 3 times with `multiFrameIndex=1,2,3`
- Same backbuffer for all 3 calls — DLSS-G generates frames at t=1/4, t=2/4, t=3/4
- Each call writes to `pOutputInterpFrame` — use different output texture per call
- Confirmed by dxvk-remix source code

### HDR Color Handling (CONFIRMED ✅ — key advantage over RIFE)
- `colorBuffersHDR=1` = scene-linear float input (NOT PQ/HLG)
- Our EXR is already scene-linear — NO tonemap/inverse-tonemap pipeline needed
- Color preservation ("补帧后颜色不变") naturally satisfied
- No ACES2/OCIO needed

## Open Questions (RESOLVED)
1. ~~Can DLSS-FG work without a swapchain?~~ → **YES**, raw NGX on VkCommandBuffer works
2. ~~What Vulkan formats does DLSS-G accept?~~ → **R16G16B16A16_SFLOAT confirmed** with colorBuffersHDR=1
3. **How to provide camera matrices from Blender?** → Need Blender script enhancement to export per-frame camera data (matrix_world, projection, FOV, aspect)
4. ~~RTX 30-series support?~~ → **NO**, RTX 40+ only
5. ~~RTX 50 for 4x?~~ → **YES**, RTX 50 only for multi-frame

## Remaining Open Question — RESOLVED
- **Camera data export**: → **JSON sidecar file** (single `camera.json` alongside EXR sequence with per-frame entries)
  - Format: `{ "frames": { "0001": { "matrix_world": [...], "projection": [...], "fov": ..., "aspect": ..., "near": ..., "far": ... }, ... } }`
  - Easy to parse in C++ (nlohmann/json already in project), human-readable for debugging

## Scope Boundaries
- INCLUDE: DLSS Frame Generation integration, CLI --interpolate flag, camera data pipeline
- INCLUDE: Blender Python script enhancement for camera export
- INCLUDE: DLSS 4 multi-frame (4x) support if hardware allows
- EXCLUDE: RIFE integration (replaced by DLSS-G)
- EXCLUDE: Tonemap/inverse-tonemap pipeline (not needed with DLSS-G)
- EXCLUDE: ACES2/OCIO integration (not needed)

## Test Strategy Decision
- **Infrastructure exists**: YES (Catch2, 9 test files)
- **Automated tests**: YES (tests after implementation)
- **Framework**: Catch2 (existing)
- **Agent-Executed QA**: ALWAYS (mandatory for all tasks)

## UE5.7 Plugin Analysis
- UE5 uses Streamline for DLSS-G, NOT raw NGX
- Streamline provides sl_dlss_g.h with higher-level abstractions
- We should NOT use Streamline — it's designed for game loops with swapchains
- The raw NGX API (which we already use for DLSS-RR) is the right approach
