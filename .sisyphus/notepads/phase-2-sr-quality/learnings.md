# Phase 2 SR Quality — Learnings

## 2026-05-07 — Initial Codebase Analysis

### File structure
- `blender/dlss_compositor_aov/__init__.py` — Main Blender extension (extension format, no bl_info)
- `blender/aov_export_preset.py` — Legacy standalone addon with bl_info
- `src/core/channel_mapper.h/cpp` — Maps EXR channels to MappedBuffers struct
- `src/core/exr_reader.cpp` — EXR alias mappings (lines 33-63)
- `src/dlss/dlss_sr_processor.h/cpp` — DLSS SR eval, currently has GBuffer paths
- `src/dlss/ngx_wrapper.h/cpp` — NGX init, createDlssSR, preset logic
- `src/cli/config.h` — AppConfig, DlssSRPreset enum, DlssQualityMode enum
- `src/cli/cli_parser.cpp` — CLI argument parsing
- `src/pipeline/sequence_processor.cpp` — Main pipeline, GBuffer texture uploads lines 763-806, 1160-1210, 1303-1360
- `src/core/version.h` — appver::kEngineName, kEngineVersion, kNgxProjectId, kDlssSdkVersion
- `src/gpu/pq_transfer_processor.h/cpp` — Template for SharpenProcessor pattern

### Current GBuffer situation
- `channel_mapper.h` structs `ChannelNames` has: diffR/G/B, specR/G/B, normalX/Y/Z, roughness
- `MappedBuffers` has: diffuseAlbedo, specularAlbedo, normals, roughness
- `channel_mapper.cpp` mapFromExr() lines 81-118 fill these
- `dlss_sr_processor.cpp` lines 121-179: conditional GBuffer validation + resource creation
- `sequence_processor.cpp` has GBuffer texture ops in multiple pipeline paths (SR-only, SRFG, SRFG per-frame)
- `DlssSRFrameInput` has 6 GBuffer fields: diffuseAlbedo/diffuseView/normals/normalsView/roughness/roughnessView

### Current Preset situation
- `config.h` DlssSRPreset enum: J=10, K=11, L=12, M=13 — NO Auto value yet
- Default preset is `DlssSRPreset::L` (line 56 in config.h)
- `ngx_wrapper.cpp` lines 169-175: sets same preset value for ALL quality modes (anti-pattern)
- Need to add Auto=0 enum value and logic: DLAA/Quality/Balanced/UltraQuality=K, Performance=M, UltraPerformance=L

### Current Banner situation
- `ngx_wrapper.cpp` init() does NOT output any banner after success
- `version.h` has kDlssSdkVersion = "310.5.3", kEngineName = "dlss-compositor", kEngineVersion = "0.2.1"
- Need to add banner output AFTER successful init

### Test structure
- `tests/test_channel_mapper.cpp` — tests for channel mapper (must update to remove GBuffer)
- `tests/test_cli.cpp` — CLI tests (must update for new flags)
- `tests/test_dlss_fg_processor.cpp` — DLSS FG tests (do NOT touch FG path)
- Build system: CMake, ctest
- Language: C++17

### Key invariants (MUST NOT CHANGE)
- Color/Depth/MV channels in channel mapper — NEVER delete
- jitter handling in blender and pipeline — NEVER touch
- FG pipeline GBuffer — FG never used GBuffer, so FG path doesn't need changes
- mv_converter / pq_transfer / lut_processor — NEVER touch
- `NGX_VULKAN_EVALUATE_DLSS_EXT` call signature — only remove GBuffer filling, keep the call

### Pattern: Compute Processor (for Sharpen task)
- Use `pq_transfer_processor.h/cpp` and `pq_transfer.comp` as templates
- Structure: VkShaderModule, VkDescriptorSetLayout, VkPipelineLayout, VkPipeline, VkDescriptorPool
- Method: createPipeline(spvPath, errorMsg), apply(cmdBuf, ...), destroy(), isReady()
- CMakeLists: compile .comp → .spv via glslangValidator or glslc, embed as resource

### Blender extension vs addon
- `blender/dlss_compositor_aov/__init__.py` = Extension format (Blender 4.2+), no bl_info at top
- `blender/aov_export_preset.py` = Old-style addon with bl_info dict
- Both need parallel changes for GBuffer removal
