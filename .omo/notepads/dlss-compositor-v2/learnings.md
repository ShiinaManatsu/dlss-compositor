# Learnings — dlss-compositor-v2

## [2026-06-02] Session Start

### Architecture
- Three-layer: Blender extension + Flutter Windows GUI + C++ Vulkan/NGX engine
- C++ targets: dlss_engine_lib (static) + dlss-compositor-plugin.dll (Flutter FFI) + dlss-compositor-cli.exe (JSON-RPC)
- Schema single source of truth: src/core/dlssconfig.schema.json

### DLSS Key Constants
- SR InMVScale = (1.0f, 1.0f) — NOT (1/W, 1/H)
- FG MvecScaleX/Y = (1.0f, 1.0f)
- MV mapping: DLSS_MV = (Blender.X, -Blender.Y)
- SR flags: IsHDR | MVLowRes | AutoExposure — NO DepthInverted
- Halton(2,3) base=8; phases: DLAA=8, UQ=8, Q=18, B=24, P=32, UP=72
- DLSS ratio: DLAA=1.0, UQ=1.0, Q=1.5, B=1.724, P=2.0, UP=3.0
- Presets: DLAA/Q/B/UQ->K; P->M; UP->L
- FG backbuffer: VK_FORMAT_A2B10G10R10_UNORM_PACK32
- FG ResourceNeverProvided: HUDLess|UI|UIAlpha|BidirectionalDistortionField
- DLSS 310.6.0: pNoPostProcessingColor REMOVED; ResourceAlwaysProvided/NeverProvided_Flags ADDED

### Build
- vcpkg manifest mode, x64-windows-static-md triplet
- glslc from Vulkan SDK for shaders -> .spv
- Debug: nvsdk_ngx_d_dbg.lib; Release: nvsdk_ngx_d.lib
