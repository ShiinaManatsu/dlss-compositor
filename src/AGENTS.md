# src/ — C++ Module Layout

**Generated:** 2026-05-09
**Commit:** 13a1b50
**Branch:** opus-rework

## OVERVIEW

Pure compute C++ batch processor. Flat namespace (`namespace Log`, `namespace appver`), module-by-function layout. No swapchain/GLFW — the C++ GUI was removed in Phase 0.

## STRUCTURE

| Module | Files | Purpose |
|--------|-------|---------|
| `cli/` | `cli_parser.cpp` (503 LOC), `cli_parser.h`, `config.h` | Argument parsing → `AppConfig` struct. Static class `CliParser` with `parse()` + `printHelp()` + `printVersion()`. Enums: `DlssQualityMode`, `DlssSRPreset`, `TonemapMode`, `ProgressFormat`, `OutputPass`. |
| `core/` | 16 files (8 .h + 8 .cpp) | CPU-only data: EXR I/O, channel mapping, motion-vector conversion, camera JSON loader, file logger, NDJSON emitter, version constants. Pure data structures — no GPU/Vulkan. |
| `dlss/` | 6 files | NGX direct-API wrapper (`ngx_wrapper.cpp:445`) + DLSS-SR processor (`dlss_sr_processor.cpp:135`) + DLSS-FG processor (`dlss_fg_processor.cpp:182`). **Not** Streamline. |
| `gpu/` | 13 files | Vulkan context (`vulkan_context.cpp:497`), texture pool (`texture_pool.cpp:383`), texture pipeline (`texture_pipeline.cpp:600`), LUT processor, PQ transfer processor, CAS sharpen processor. Compute-only (no swapchain). |
| `pipeline/` | 7 files | Orchestration: `sequence_processor.cpp` (1949 LOC), async EXR writer, frame prefetcher. |
| `shaders/` | 3 `.comp` files | GLSL compute: `apply_lut.comp`, `pq_transfer.comp`, `cas_sharpen.comp`. Compiled to SPIR-V at build time via `glslc`. |
| `main.cpp` | 135 LOC | Entry point: parse flags → init Vulkan + NGX → dispatch to `SequenceProcessor`. Five exit paths: help, version, test-vulkan, test-ngx, process-directory. |

## DATA FLOW

```
CLI args ──→ CliParser::parse() ──→ AppConfig
                                       │
                                       ▼
                              SequenceProcessor::processDirectory()
                                       │
                              ┌────────┼────────┐
                              ▼        ▼        ▼
                        SR-only    FG-only   SR+FG
                       (inline,   (process   (process
                        L696+)    Directory  Directory
                                  FG L1733)  SRFG L1050)
                              │        │        │
                              └────────┼────────┘
                                       ▼
                         ChannelMapper → TexturePool
                              │
                              ▼
                    NGX SR evaluate() → NGX FG evaluate()
                              │
                              ▼
                       AsyncExrWriter (thread pool I/O)
```

Three pipelines coexist in `sequence_processor.cpp`: SR-only (inline in `processDirectory` L696+), FG-only (`processDirectoryFG` L1733), SR+FG (`processDirectorySRFG` L1050). Dispatch at `sequence_processor.cpp:689-694`.

## MODULE BOUNDARIES

| Module | Depends on | Never called from |
|--------|-----------|-------------------|
| `cli/` | `core/` (config includes `exr_writer.h`, `nvsdk_ngx_defs.h`) | `pipeline/` (one-way dependency) |
| `core/` | OpenEXR, nlohmann_json, STL only | nothing with GPU/Vulkan |
| `gpu/` | volk, VMA, `core/` | `core/` (GPU cannot leak to CPU-only layer) |
| `dlss/` | `gpu/` + NGX SDK headers (`nvsdk_ngx_*.h`) | `core/` (NGX headers must not reach CPU layer) |
| `pipeline/` | All other modules | nowhere else (leaf orchestrator) |

Dependency chain: `cli/` → `core/` → `gpu/` → `dlss/` ← `pipeline/`

`pipeline/` is the only module that touches all others. It should remain the sole cross-cutter.

## KEY CONVENTIONS

### C++ style
- `m_` prefix for all non-static member variables (`vulkan_context.h:36-48`). Static members: `s_`.
- Classes `PascalCase`; methods `camelCase`; files `snake_case`; enums `PascalCase`.
- No `using namespace std;` anywhere in the codebase (verified).
- No exceptions for expected failures — use `bool func(..., std::string& errorMsg)` pattern (`dlss_sr_processor.h:39`, `ngx_wrapper.h:27`).
- `std::filesystem::path` for paths (32 usages across 5 files, `sequence_processor.h:13`).
- PIMPL for heavyweight classes behind platform-specific headers: `ExrReader` stores `std::unique_ptr<Impl> m_impl` (`exr_reader.h:31-32`).

### Vulkan
- Always include `<volk.h>` (never `<vulkan/vulkan.h>` directly). `vulkan_context.h:7`.
- `#define VK_USE_PLATFORM_WIN32_KHR` before `volk.h` in every GPU-facing header (`vulkan_context.h:3-4`, `dlss_sr_processor.h:3-4`).
- Vulkan handles are stored as `VkImage + VkImageView + VkFormat` triplets (`DlssSRFrameInput` struct, `dlss_sr_processor.h:14-33`). Dedicated `GpuResource` struct TBD.

### Logging & NDJSON
- `Log::info()` → stdout + log file; `Log::error()` → stderr + log file (`logger.h:10-13`).
- `Log::setQuietStdout(true)` routes info to stderr when `--progress-format json` is active (`main.cpp:31`). This keeps stdout reserved for NDJSON.
- NDJSON events emitted via `globalNdjsonEmitter().emitRaw()` — thread-safe singleton (`ndjson_emitter.h:36`).

### Versioning
- All version constants in `core/version.h` (`appver::` namespace, 4 constants). Extend in place — never redefine in `.cpp` files.

### Large file exception
- `sequence_processor.cpp` is 1949 LOC with 3 pipelines sharing helpers. Refactor candidate to extract shared SR/FG setup into dedicated helpers.

## ANTI-PATTERNS

- ❌ Include `<vulkan/vulkan.h>` directly → use `<volk.h>`.
- ❌ Include NGX headers from `core/`. NGX dependency stops at `dlss/`.
- ❌ `Log::info()` on stdout when `--progress-format json`. Logger routes to stderr; only NDJSON touches stdout.
- ❌ Duplicate version constants from `core/version.h`. Single source in `appver::`.
- ❌ Populate `evalParams.GBufferSurface` for DLSS-SR. Verified absent from all src/ files (deleted Phase 2 — NVIDIA marks GBuffer as "research only" in `nvsdk_ngx_helpers_vk.h:89-90`).

## TESTS

Test files live in `tests/`. CPU-only tests for `core/` (no Vulkan needed): `test_exr_reader.cpp`, `test_mv_converter.cpp`, `test_channel_mapper.cpp`, `test_camera_data_loader.cpp`, `test_cli.cpp`, `test_ndjson_emitter.cpp`. GPU smoke tests require RTX: `test_texture_pool.cpp`, `test_texture_pipeline.cpp`, `test_dlss_fg_processor.cpp`, `test_dlss_rr_processor.cpp`, `test_sequence_processor.cpp`, `test_integration.cpp`. Run with `ctest --test-dir build --config Release --output-on-failure`.
