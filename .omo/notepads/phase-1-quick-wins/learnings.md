# Learnings — phase-1-quick-wins

## 2026-05-07 Session Start: Codebase Analysis

### Key file locations
- src/cli/cli_parser.cpp — CLI flag parsing, printHelp(), printVersion()
- src/cli/config.h — AppConfig struct, enums
- src/dlss/ngx_wrapper.cpp — NgxContext::init, NGX log level at line 103
- src/gpu/vulkan_context.cpp — Second copy of kNgxEngineVersion at line 29 ("0.1.0")
- src/dlss/dlss_sr_processor.cpp — InFrameTimeDeltaInMsec hardcoded at line 158
- src/dlss/dlss_sr_processor.h — DlssSRFrameInput struct
- src/pipeline/sequence_processor.cpp — SR frame setup at line 794, SR+FG at ~1340
- src/core/camera_data_loader.h/.cpp — Camera data parsing
- blender/dlss_compositor_aov/__init__.py — Blender addon (jitter lines 75-80, export_camera 156-190)
- blender/aov_export_preset.py — Standalone addon (same jitter, NOT @persistent decorated)

### Task 1 (version constants)
- ngx_wrapper.cpp:32: kNgxEngineVersion = "0.2.1" (correct)
- vulkan_context.cpp:29: kNgxEngineVersion = "0.1.0" (WRONG - inconsistent!)
- Both files also have kNgxProjectId duplicated
- cli_parser.cpp printHelp/printVersion hardcodes "0.1.0" - should update to appver::

### Task 2 (tonemap custom)
- TonemapMode::Custom exists in config.h
- parseTonemapMode() MISSING "custom" case
- Error msg: "--tonemap must be one of: none, pq" - needs "custom"
- Validation at lines 404-414 already handles Custom correctly

### Task 3 (NGX log level)
- ngx_wrapper.cpp:103: NVSDK_NGX_LOGGING_LEVEL_VERBOSE hardcoded
- NgxContext::init() has 5 params - need to add log level or use AppConfig
- main.cpp calls ngx.init() in two places

### Task 4 (source-fps)
- dlss_sr_processor.cpp:158: 33.3f hardcoded
- DlssSRFrameInput needs frameTimeDeltaMs field
- sequence_processor.cpp line 794 and ~1340 need to set this field

### Task 5/6 (jitter)
- apply_jitter() uses frame_num % 8 hardcoded
- export_camera recalculates independently
- Need _runtime_jitter dict for consistency
