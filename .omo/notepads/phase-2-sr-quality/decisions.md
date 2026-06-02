# Phase 2 SR Quality — Decisions

## 2026-05-07 — Architecture Decisions

### GBuffer Removal Approach
- Delete ALL 4 GBuffer passes from Blender (Normal, DiffuseColor, GlossyColor, Roughness AOV)
- Delete corresponding fields from ChannelNames, MappedBuffers, DlssSRFrameInput
- Delete ALL GBuffer texture uploads from ALL pipeline paths in sequence_processor.cpp
- Delete GBuffer validation from dlss_sr_processor.cpp (the optional checks at lines 121-132)
- Delete GBuffer resource creation (albedoResource/normalsResource/roughnessResource variables)
- Delete GBufferSurface.pInAttrib assignments — keep evalParams structure intact, just don't fill GBuffer fields
- Delete EXR aliases for DiffCol, GlossCol, Normal, Roughness in exr_reader.cpp (lines 44-61 = Normal, 50-54 = DiffCol/GlossCol, 59-60 = Roughness)
- Add cleanup operator in Blender for legacy scenes

### Preset Auto Approach
- Add `Auto = 0` to DlssSRPreset enum (as first value, before J=10)
- Change default preset to Auto in AppConfig
- In ngx_wrapper createDlssSR: use lambda `pickPreset(def)` that returns def-enum-int when Auto, or explicit preset int otherwise
- Log the effective preset for each quality mode

### Banner Approach
- Add banner after `m_initialized = true` in NgxContext::init()
- Use Log::info for all [NGX] lines
- DLL version query: use GetFileVersionInfoW API (Windows-specific, #ifdef _WIN32 guard)
- Feature creation banner: add after NGX_VULKAN_CREATE_DLSS_EXT1 success in createDlssSR

### Sharpen Approach  
- New file: `src/shaders/cas_sharpen.comp` — simple unsharp mask (AMD CAS-inspired)
- New files: `src/gpu/sharpen_processor.h/cpp` — follows PqTransferProcessor pattern exactly
- Add `float sharpness = 0.0f` to AppConfig, `--sharpness` flag to CLI
- Integration point in sequence_processor.cpp: after SR evaluate, before download, if sharpness > 0.0
- Default 0.0 = bypass (no sharpen call)
- CMakeLists: compile cas_sharpen.comp → spv

### Wave Execution Order
1. Wave 1 (parallel): T1 Blender GBuffer, T2 C++ GBuffer, T3 Preset, T4 Banner
2. Wave 2: T5 Sharpen  
3. Wave 3: T6 A/B verify, T7 e2e verify
4. Final: F1-F4 reviewers
