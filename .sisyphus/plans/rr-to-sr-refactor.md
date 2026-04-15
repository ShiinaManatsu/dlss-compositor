# DLSS RR→SR 重构：Super Resolution 替代 Ray Reconstruction

## TL;DR

> **Quick Summary**: 将 dlss-compositor 的核心超分特性从 DLSS Ray Reconstruction (RR) 重构为 DLSS Super Resolution (SR)。RR 在后处理 EXR 场景下因缺少 hit distance/ray direction 数据而不可用，SR 只需 color+depth+motionVectors 即可工作。同时新增 `--preset` CLI 选项（默认 L），更新 GUI 和 Blender addon。
> 
> **Deliverables**:
> - C++ 核心：SR 创建/评估替代 RR，新增 preset 支持
> - CLI：`--preset` 参数，help text 更新
> - GUI：preset 选择器，所有 RR 引用→SR
> - Blender addon：简化 AOV 配置（SR 只需 Combined/Z/Vector）
> - 文档：README + usage guide 全面更新
> - 构建验证：cmake Release 构建通过
> 
> **Estimated Effort**: Medium
> **Parallel Execution**: YES - 4 waves
> **Critical Path**: Task 1 (types) → Task 2 (ngx_wrapper SR) → Task 3 (SR processor) → Task 5 (sequence_processor pipelines) → Task 8 (build verify)

---

## Context

### Original Request
用户发现 RR 在 Blender EXR 后处理场景下不可用（缺少 DiffuseHitDistance、SpecularHitDistance、DiffuseRayDirection、SpecularRayDirection），要求将核心超分方案从 RR 替换为 SR。保留 FG 和 PQ transport，默认预设 L。

### Interview Summary
**Key Discussions**:
- RR 完全移除，不保留为可选模式
- SR 传入可选 GBuffer hints（albedo, normals, roughness）增强质量——EXR 有就传，没有不报错
- Pipeline 命名更新：SR / SRFG / FG（替代 RR / RRFG）
- SR 默认预设 L（用户明确要求 `NVSDK_NGX_DLSS_Hint_Render_Preset_L`）
- 不需要单元测试，验证方式：cmake 构建 + Playwright e2e + GUI 实际运行

**Research Findings**:
- SR 使用 `NVSDK_NGX_Feature_SuperSampling` 创建，RR 使用 `NVSDK_NGX_Feature_RayReconstruction`
- SR create params: `NVSDK_NGX_DLSS_Create_Params`（定义在 `nvsdk_ngx_params.h:37-43`）
- SR eval params: `NVSDK_NGX_VK_DLSS_Eval_Params`（定义在 `nvsdk_ngx_helpers_vk.h:64-100`）
- SR 可用性查询：`NVSDK_NGX_Parameter_SuperSampling_Available`（defs.h:616）
- SR preset 通过参数集设置：`NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_<QualityMode>, presetValue)`
- `NGX_VULKAN_CREATE_DLSS_EXT1` helper 可用于简化 SR 创建（`nvsdk_ngx_helpers_vk.h:113-134`）
- `NGX_VULKAN_EVALUATE_DLSS_EXT` helper 可用于简化 SR 评估（`nvsdk_ngx_helpers_vk.h:147+`）
- GBuffer hints 在 SR eval 中通过 `GBufferSurface.pInAttrib[NVSDK_NGX_GBUFFER_ALBEDO/ROUGHNESS/NORMALS]` 传入
- `NVSDK_NGX_DLSS_Create_Params` 结构体：包含 `Feature`（InWidth, InHeight, InTargetWidth, InTargetHeight, InPerfQualityValue）+ `InFeatureCreateFlags` + `InEnableOutputSubrects`

### Metis Review
**Identified Gaps** (addressed):
- SR preset 必须 per-quality-mode 设置（e.g. `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA`），不是全局一个——已纳入方案
- SR 不需要 specularAlbedo（只有 albedo/normals/roughness 可选）——DlssFrameInput 简化
- RR availability 变 SR availability 需要不同的参数名——已确认 `NVSDK_NGX_Parameter_SuperSampling_Available`
- 构建验证应覆盖 SR-only、FG-only、SRFG 三种模式——已纳入 QA 场景
- Pipeline 日志 prefix 需要统一更新——已纳入

---

## Work Objectives

### Core Objective
将 DLSS 超分引擎从 Ray Reconstruction 完全替换为 Super Resolution，新增 preset 支持，保持 FG 和 PQ transport 不变。

### Concrete Deliverables
- `src/cli/config.h` — 新增 `DlssSRPreset` enum 和 `AppConfig::preset` 字段
- `src/cli/cli_parser.cpp` — 新增 `--preset` 参数解析和 help text 更新
- `src/dlss/ngx_wrapper.h` / `.cpp` — `createDlssSR()` 替代 `createDlssRR()`，`queryDlssSRAvailability()` 替代 `queryDlssRRAvailability()`
- `src/dlss/dlss_sr_processor.h` / `.cpp` — 新文件，SR eval 替代 RR eval
- `src/pipeline/sequence_processor.cpp` — 三个 pipeline 函数使用 SR
- `gui/src/types/dlss-config.ts` — 新增 `preset` 字段
- `gui/src/state/config-store.ts` — 新增 `SET_PRESET` action
- `gui/src/components/BasicSettings.tsx` — 新增 preset 选择器
- `gui/electron/cli-args.ts` — 新增 `--preset` 参数构建
- `README.md` — 所有 RR 引用→SR
- 删除 `src/dlss/dlss_rr_processor.h` 和 `src/dlss/dlss_rr_processor.cpp`

### Definition of Done
- [ ] `cmake --build build --config Release` 编译成功，零错误
- [ ] 代码中零 `RayReconstruction`、`DLSSD`（DLSS-FG 的 DLSSG 相关不算）、`dlss_rr` 引用
- [ ] `--preset` CLI 参数可解析 J/K/L/M
- [ ] GUI preset 选择器可见且可操作

### Must Have
- SR 创建使用 `NVSDK_NGX_Feature_SuperSampling` + `NVSDK_NGX_DLSS_Create_Params`
- SR 评估使用 `NVSDK_NGX_VK_DLSS_Eval_Params` + `NGX_VULKAN_EVALUATE_DLSS_EXT`
- SR 可用性查询使用 `NVSDK_NGX_Parameter_SuperSampling_Available`
- SR 默认预设 L
- GBuffer hints（albedo, normals, roughness）作为可选增强传入 SR eval
- FG 逻辑完全不改（`createDlssFG`, `releaseDlssFG`, `DlssFgProcessor` 不动）
- PQ transport 完全不改

### Must NOT Have (Guardrails)
- 不引入任何新的 RR 代码路径或 "RR 可选模式"
- 不修改 FG 创建/评估/释放逻辑
- 不修改 PQ transport 编解码逻辑
- 不修改 EXR 读写框架（`exr_reader.h/cpp`, `exr_writer.h/cpp`）
- 不引入新的第三方依赖
- 不添加单元测试框架
- 不改动 `channel_mapper`、`mv_converter` 内部逻辑
- 不在 SR eval 中硬性要求 GBuffer（缺少时不报错，只跳过）
- 不添加过度注释（每个函数最多 1 行描述注释）

---

## Verification Strategy (MANDATORY)

> **ZERO HUMAN INTERVENTION** - ALL verification is agent-executed. No exceptions.

### Test Decision
- **Infrastructure exists**: NO
- **Automated tests**: NO
- **Framework**: none
- **Verification**: cmake Release 构建 + Playwright e2e + GUI 运行 CLI

### QA Policy
Every task MUST include agent-executed QA scenarios.
Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **C++ Build**: Use Bash — `cmake --build build --config Release`
- **CLI**: Use Bash — `./build/Release/dlss-compositor.exe --help`、`--preset` 参数验证
- **GUI/Frontend**: Use Playwright — 导航、选择 preset、验证 CLI 参数构建
- **Code Audit**: Use Bash (grep) — 验证零残留 RR 引用

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Foundation — types, enums, config):
├── Task 1: C++ types + config (config.h + cli_parser.cpp) [quick]
├── Task 4: GUI types + state (dlss-config.ts + config-store.ts) [quick]
└── Task 9: Blender addon AOV 简化 [quick]

Wave 2 (Core C++ — depends on Task 1):
├── Task 2: ngx_wrapper SR 创建/释放/可用性 (depends: 1) [deep]
├── Task 3: dlss_sr_processor 新建 (depends: 1) [deep]
└── Task 6: GUI preset 控件 + cli-args (depends: 4) [visual-engineering]

Wave 3 (Integration — depends on Wave 2):
├── Task 5: sequence_processor 三个 pipeline 更新 (depends: 2, 3) [deep]
├── Task 7: CLI help text + stdout format + cleanup 残留 (depends: 2) [quick]
└── Task 10: 文档更新 README + guides (depends: 5) [writing]

Wave 4 (Verification):
├── Task 8: 构建验证 + 代码审计 (depends: all) [unspecified-high]

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real manual QA (unspecified-high)
└── Task F4: Scope fidelity check (deep)
-> Present results -> Get explicit user okay

Critical Path: Task 1 → Task 2 → Task 3 → Task 5 → Task 8 → F1-F4 → user okay
Parallel Speedup: ~55% faster than sequential
Max Concurrent: 3 (Wave 1 and Wave 2)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 2, 3, 5, 7 | 1 |
| 4 | — | 6 | 1 |
| 9 | — | 10 | 1 |
| 2 | 1 | 5, 7 | 2 |
| 3 | 1 | 5 | 2 |
| 6 | 4 | 8 | 2 |
| 5 | 2, 3 | 8, 10 | 3 |
| 7 | 2 | 8 | 3 |
| 10 | 5, 9 | 8 | 3 |
| 8 | 5, 6, 7, 10 | F1-F4 | 4 |
| F1-F4 | 8 | — | FINAL |

### Agent Dispatch Summary

- **Wave 1**: **3 tasks** — T1 → `quick`, T4 → `quick`, T9 → `quick`
- **Wave 2**: **3 tasks** — T2 → `deep`, T3 → `deep`, T6 → `visual-engineering`
- **Wave 3**: **3 tasks** — T5 → `deep`, T7 → `quick`, T10 → `writing`
- **Wave 4**: **1 task** — T8 → `unspecified-high`
- **FINAL**: **4 tasks** — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. C++ Types & Config: Add DlssSRPreset enum and --preset CLI option

  **What to do**:
  - In `src/cli/config.h`:
    - Add `enum class DlssSRPreset { J = 10, K = 11, L = 12, M = 13 };` (values match `NVSDK_NGX_DLSS_Hint_Render_Preset` enum in `nvsdk_ngx_defs.h:70-88`)
    - Add `DlssSRPreset preset = DlssSRPreset::L;` field to `AppConfig` struct (after the `quality` field)
  - In `src/cli/cli_parser.cpp`:
    - Add `parsePreset()` helper function (same pattern as `parseQuality()` at line 43-65). Accept strings: `J`, `K`, `L`, `M` (case-sensitive)
    - Add `--preset` argument handling in `CliParser::parse()` (insert after the `--quality` block at line 270-279)
    - Update `printHelp()` to add `--preset <mode>` line: `"  --preset <mode>        SR preset hint (J|K|L|M; default: L)\n"`
    - Change `--test-ngx` help text from "Test NGX/DLSS-RR availability" to "Test NGX/DLSS-SR availability"

  **Must NOT do**:
  - Do not modify any enum values in `DlssQualityMode` — it stays as-is
  - Do not add a `preset` field to `DlssConfig` TypeScript type yet (that's Task 4)
  - Do not change the `--quality` parameter behavior

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Two-file change, adding an enum and a CLI option following existing patterns
  - **Skills**: []
    - No special skills needed — straightforward C++ edits
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not a UI task

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 4, 9)
  - **Blocks**: Tasks 2, 3, 5, 7
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `src/cli/config.h:8-14` — `DlssQualityMode` enum pattern (follow same style for `DlssSRPreset`)
  - `src/cli/config.h:40-75` — `AppConfig` struct (insert `preset` field after line 46)
  - `src/cli/cli_parser.cpp:43-65` — `parseQuality()` function (copy this pattern for `parsePreset()`)
  - `src/cli/cli_parser.cpp:270-279` — `--quality` argument parsing (copy this pattern for `--preset`)
  - `src/cli/cli_parser.cpp:408-436` — `printHelp()` function (add `--preset` line)

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_defs.h:70-88` — `NVSDK_NGX_DLSS_Hint_Render_Preset` enum with values J=10, K=11, L=12, M=13 (SR preset values must match these)

  **WHY Each Reference Matters**:
  - `config.h` enums — Follow exact same `enum class` pattern for consistency
  - `parseQuality()` — The preset parser is functionally identical, just different string→enum mapping
  - `--quality` handling — The `--preset` block is structurally identical

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with new preset enum
    Tool: Bash
    Preconditions: cmake already configured
    Steps:
      1. Run: cmake --build build --config Release
      2. Check exit code is 0
    Expected Result: Build succeeds with zero errors
    Failure Indicators: Non-zero exit code, "error:" in output
    Evidence: .sisyphus/evidence/task-1-build-success.txt

  Scenario: --preset L parses correctly (happy path)
    Tool: Bash
    Preconditions: Build completed
    Steps:
      1. Run: ./build/Release/dlss-compositor.exe --help
      2. Verify output contains "--preset"
      3. Verify output contains "J|K|L|M"
    Expected Result: --preset documented in help output
    Failure Indicators: help output missing --preset line
    Evidence: .sisyphus/evidence/task-1-help-output.txt

  Scenario: --preset with invalid value fails gracefully
    Tool: Bash
    Preconditions: Build completed
    Steps:
      1. Run: ./build/Release/dlss-compositor.exe --preset Z 2>&1
      2. Check exit code is non-zero
      3. Verify stderr contains error message about invalid preset
    Expected Result: Clear error message, non-zero exit
    Failure Indicators: Silent acceptance of invalid preset
    Evidence: .sisyphus/evidence/task-1-invalid-preset.txt
  ```

  **Commit**: YES (group 1)
  - Message: `feat(config): add DlssSRPreset enum and --preset CLI option`
  - Files: `src/cli/config.h`, `src/cli/cli_parser.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 2. ngx_wrapper: Replace RR with SR creation, release, and availability

  **What to do**:
  - In `src/dlss/ngx_wrapper.h`:
    - Rename `isDlssRRAvailable()` → `isDlssSRAvailable()`
    - Rename `createDlssRR()` → `createDlssSR()` — add `DlssSRPreset preset` parameter after `quality`
    - Rename `releaseDlssRR()` → `releaseDlssSR()`
    - Rename `queryDlssRRAvailability()` → `queryDlssSRAvailability()`
    - Rename `m_dlssRRAvailable` → `m_dlssSRAvailable`
    - Update `unavailableReason()` string references from "RR" to "SR"
    - Add `#include <nvsdk_ngx_helpers_vk.h>` (for `NGX_VULKAN_CREATE_DLSS_EXT1`)
  - In `src/dlss/ngx_wrapper.cpp`:
    - **createDlssSR()** (was createDlssRR, line 148-207): Complete rewrite of the function body:
      - Use `NVSDK_NGX_DLSS_Create_Params` (from `nvsdk_ngx_params.h:37-43`) instead of `NVSDK_NGX_DLSSD_Create_Params`
      - Set `createParams.Feature.InWidth/InHeight/InTargetWidth/InTargetHeight/InPerfQualityValue`
      - Set `createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes`
      - Set `createParams.InEnableOutputSubrects = false`
      - Remove RR-specific params: `Denoise_Mode`, `Roughness_Mode`, `Use_HW_Depth` — do NOT set these
      - Set SR preset via params **before** create: for each quality mode, set `NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<int>(preset))` and similarly for `_Quality`, `_Balanced`, `_Performance`, `_UltraPerformance`, `_UltraQuality` (set ALL modes to the same preset value — this ensures the preset applies regardless of which quality mode is selected)
      - Call `NGX_VULKAN_CREATE_DLSS_EXT1(m_device, cmdBuf, 1, 1, &m_featureHandle, m_parameters, &createParams)` — this uses `NVSDK_NGX_Feature_SuperSampling` internally (see `nvsdk_ngx_helpers_vk.h:132`)
      - **ALTERNATIVELY** can set params manually and call `NVSDK_NGX_VULKAN_CreateFeature1(m_device, cmdBuf, NVSDK_NGX_Feature_SuperSampling, m_parameters, &m_featureHandle)` directly (current RR code does this at line 195-199). Either approach works. Using the helper is cleaner.
    - **releaseDlssSR()** (was releaseDlssRR, line 256-263): Just rename, logic stays the same
    - **queryDlssSRAvailability()** (was queryDlssRRAvailability, line 336-386): 
      - Change `NVSDK_NGX_Parameter_SuperSamplingDenoising_Available` → `NVSDK_NGX_Parameter_SuperSampling_Available` (defined in `nvsdk_ngx_defs.h:616`)
      - Remove fallback `kRayReconstructionAvailableParam` (line 345) — not needed for SR
      - Change all `NVSDK_NGX_Parameter_SuperSamplingDenoising_*` parameters to `NVSDK_NGX_Parameter_SuperSampling_*` equivalents (NeedsUpdatedDriver, MinDriverVersionMajor, MinDriverVersionMinor, FeatureInitResult — all at defs.h:623-646)
      - Update error strings: "DLSS-RR" → "DLSS-SR"
    - Remove `#include <nvsdk_ngx_defs_dlssd.h>` and `#include <nvsdk_ngx_helpers_dlssd_vk.h>` includes if present
    - Add `#include <nvsdk_ngx_helpers_vk.h>` if using the helper function

  **Must NOT do**:
  - Do not touch `createDlssFG()`, `releaseDlssFG()`, `queryDlssFGAvailability()` — FG stays unchanged
  - Do not rename `m_featureHandle` (it's shared between SR and the old RR path conceptually)
  - Do not change `mapQuality()` — it maps the same `DlssQualityMode` enum

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Core DLSS SDK integration change, requires understanding NGX API differences
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not UI work

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 2, alongside Task 3)
  - **Parallel Group**: Wave 2 (with Tasks 3, 6)
  - **Blocks**: Tasks 5, 7
  - **Blocked By**: Task 1

  **References**:

  **Pattern References**:
  - `src/dlss/ngx_wrapper.cpp:148-207` — Current `createDlssRR()` implementation (rewrite this)
  - `src/dlss/ngx_wrapper.cpp:256-263` — Current `releaseDlssRR()` (just rename)
  - `src/dlss/ngx_wrapper.cpp:336-386` — Current `queryDlssRRAvailability()` (change param names)
  - `src/dlss/ngx_wrapper.cpp:209-252` — `createDlssFG()` reference for "do not touch" boundary

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_params.h:37-43` — `NVSDK_NGX_DLSS_Create_Params` struct definition (Feature.InWidth, Feature.InHeight, etc.)
  - `DLSS/include/nvsdk_ngx_params.h:27-35` — `NVSDK_NGX_Feature_Create_Params` struct (nested in DLSS_Create_Params)
  - `DLSS/include/nvsdk_ngx_helpers_vk.h:113-134` — `NGX_VULKAN_CREATE_DLSS_EXT1` helper function (calls `NVSDK_NGX_Feature_SuperSampling`)
  - `DLSS/include/nvsdk_ngx_defs.h:616` — `NVSDK_NGX_Parameter_SuperSampling_Available` (SR availability check)
  - `DLSS/include/nvsdk_ngx_defs.h:623-646` — `NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver`, `_MinDriverVersionMajor`, `_MinDriverVersionMinor`, `_FeatureInitResult`
  - `DLSS/include/nvsdk_ngx_defs.h:70-88` — `NVSDK_NGX_DLSS_Hint_Render_Preset` enum (J=10, K=11, L=12, M=13)
  - `DLSS/include/nvsdk_ngx_defs_dlssd.h` — RR struct definitions (this file will no longer be included)

  **WHY Each Reference Matters**:
  - Current `createDlssRR()` — shows the structure to rewrite; most parameter-setting logic is similar but with different struct types and without RR-specific fields
  - `NVSDK_NGX_DLSS_Create_Params` — this is the SR create struct; it's simpler than `DLSSD_Create_Params` (no Denoise_Mode, Roughness_Mode, Use_HW_Depth)
  - `NGX_VULKAN_CREATE_DLSS_EXT1` — the clean way to create SR features; already handles parameter setting and calls `CreateFeature1` with `Feature_SuperSampling`
  - Availability params — exact string parameter names are critical; one character wrong and it silently returns false

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with SR wrapper
    Tool: Bash
    Preconditions: Task 1 completed
    Steps:
      1. Run: cmake --build build --config Release
      2. Check exit code is 0
    Expected Result: Build succeeds
    Failure Indicators: Compile errors in ngx_wrapper.cpp
    Evidence: .sisyphus/evidence/task-2-build-success.txt

  Scenario: No RR references remain in ngx_wrapper files
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "src/dlss/ngx_wrapper.*" -Pattern "DlssRR|DLSSD|RayReconstruction|SuperSamplingDenoising"
      2. Verify zero matches
    Expected Result: Zero matches
    Failure Indicators: Any match found
    Evidence: .sisyphus/evidence/task-2-no-rr-refs.txt

  Scenario: FG functions untouched
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Read src/dlss/ngx_wrapper.cpp createDlssFG function (line ~209-252)
      2. Compare with original — should be byte-identical
    Expected Result: createDlssFG, releaseDlssFG, queryDlssFGAvailability unchanged
    Failure Indicators: Any diff in FG functions
    Evidence: .sisyphus/evidence/task-2-fg-untouched.txt
  ```

  **Commit**: YES (group 2)
  - Message: `refactor(dlss): replace RR with SR in ngx_wrapper and processor`
  - Files: `src/dlss/ngx_wrapper.h`, `src/dlss/ngx_wrapper.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 3. Create dlss_sr_processor (new files replacing dlss_rr_processor)

  **What to do**:
  - Create `src/dlss/dlss_sr_processor.h`:
    - Define `DlssSRFrameInput` struct — same as current `DlssFrameInput` in `dlss_rr_processor.h:14-40` BUT:
      - Remove `specularAlbedo` / `specularView` fields (SR doesn't use specular separately)
      - Keep `diffuseAlbedo` / `diffuseView` (maps to `NVSDK_NGX_GBUFFER_ALBEDO`)
      - Keep `normals` / `normalsView` (maps to `NVSDK_NGX_GBUFFER_NORMALS`)
      - Keep `roughness` / `roughnessView` (maps to `NVSDK_NGX_GBUFFER_ROUGHNESS`)
      - All GBuffer fields are OPTIONAL — `VK_NULL_HANDLE` means "skip this hint"
    - Define `DlssSRProcessor` class with same API: constructor takes `VulkanContext&` and `NgxContext&`, has `evaluate(VkCommandBuffer, const DlssSRFrameInput&, std::string&)` method
  - Create `src/dlss/dlss_sr_processor.cpp`:
    - Copy the helper functions from `dlss_rr_processor.cpp` (validateImage, makeImageResource — lines 1-87)
    - Implement `evaluate()` method:
      - Validate required images: color, depth, motionVectors, output (fail if missing)
      - GBuffer images (diffuseAlbedo, normals, roughness): validate ONLY if not VK_NULL_HANDLE — skip silently if null
      - Create resources for required images
      - Create resources for optional GBuffer images only if present
      - Build `NVSDK_NGX_VK_DLSS_Eval_Params` struct (defined in `nvsdk_ngx_helpers_vk.h:64-100`):
        - `Feature.pInColor = &colorResource`, `Feature.pInOutput = &outputResource`
        - `pInDepth = &depthResource`, `pInMotionVectors = &motionResource`
        - `InJitterOffsetX/Y`, `InRenderSubrectDimensions`, `InReset`, `InMVScaleX/Y`
        - `InPreExposure = 1.0f`, `InExposureScale = 1.0f`, `InFrameTimeDeltaInMsec = 33.3f`
        - GBuffer hints: if `diffuseAlbedo != VK_NULL_HANDLE`, set `GBufferSurface.pInAttrib[NVSDK_NGX_GBUFFER_ALBEDO] = &albedoResource`
        - Similarly for normals (NVSDK_NGX_GBUFFER_NORMALS) and roughness (NVSDK_NGX_GBUFFER_ROUGHNESS)
      - Call `NGX_VULKAN_EVALUATE_DLSS_EXT(cmdBuf, handle, params, &evalParams)` (defined in `nvsdk_ngx_helpers_vk.h:147+`)
      - Error handling: same pattern as current RR processor
  - Delete `src/dlss/dlss_rr_processor.h` and `src/dlss/dlss_rr_processor.cpp`
  - Update `CMakeLists.txt` (if sources are listed explicitly): remove `dlss_rr_processor` entries, add `dlss_sr_processor` entries

  **Must NOT do**:
  - Do not modify `dlss_fg_processor.h/cpp` at all
  - Do not make GBuffer hints mandatory — if any GBuffer image is VK_NULL_HANDLE, skip it silently
  - Do not use `NVSDK_NGX_VK_DLSSD_Eval_Params` (that's the RR struct)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: New file creation implementing SDK eval API, requires understanding of Vulkan resource types and NGX eval param structure
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not UI work

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 2, alongside Task 2)
  - **Parallel Group**: Wave 2 (with Tasks 2, 6)
  - **Blocks**: Task 5
  - **Blocked By**: Task 1

  **References**:

  **Pattern References**:
  - `src/dlss/dlss_rr_processor.h:14-40` — Current `DlssFrameInput` struct (base for `DlssSRFrameInput`, remove specular fields)
  - `src/dlss/dlss_rr_processor.h:42-51` — Current `DlssRRProcessor` class (copy API for `DlssSRProcessor`)
  - `src/dlss/dlss_rr_processor.cpp:1-87` — Helper functions (validateImage, makeImageResource — copy as-is)
  - `src/dlss/dlss_rr_processor.cpp:91-174` — Current `evaluate()` method (rewrite with SR eval params)

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_helpers_vk.h:64-100` — `NVSDK_NGX_VK_DLSS_Eval_Params` struct (SR eval params)
  - `DLSS/include/nvsdk_ngx_helpers_vk.h:147-230` — `NGX_VULKAN_EVALUATE_DLSS_EXT` helper function
  - `DLSS/include/nvsdk_ngx_defs.h` — `NVSDK_NGX_GBUFFER_ALBEDO`, `NVSDK_NGX_GBUFFER_NORMALS`, `NVSDK_NGX_GBUFFER_ROUGHNESS` GBuffer attribute indices

  **External References**:
  - DLSS SR programming guide — GBufferSurface is optional, only enhances quality when provided

  **WHY Each Reference Matters**:
  - Current RR processor — shows the exact evaluate() flow structure to follow; SR is simpler (fewer required buffers)
  - `NVSDK_NGX_VK_DLSS_Eval_Params` — this is the SR eval struct; critically different from `DLSSD_Eval_Params` (no diffuseAlbedo/specularAlbedo as direct fields, uses GBufferSurface array instead)
  - `NGX_VULKAN_EVALUATE_DLSS_EXT` — the helper that wraps param-setting and evaluation; cleaner than manual param injection
  - GBuffer constants — exact array indices for attribs; wrong index = wrong channel = visual artifacts

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds with new SR processor
    Tool: Bash
    Preconditions: Task 1 and Task 2 completed
    Steps:
      1. Run: cmake --build build --config Release
      2. Check exit code is 0
    Expected Result: Build succeeds, new files compile
    Failure Indicators: Compile errors in dlss_sr_processor.cpp
    Evidence: .sisyphus/evidence/task-3-build-success.txt

  Scenario: Old RR processor files deleted
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Check: Test-Path src/dlss/dlss_rr_processor.h
      2. Check: Test-Path src/dlss/dlss_rr_processor.cpp
      3. Both should return False
    Expected Result: Both files deleted
    Failure Indicators: Either file still exists
    Evidence: .sisyphus/evidence/task-3-rr-deleted.txt

  Scenario: New SR processor files exist
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Check: Test-Path src/dlss/dlss_sr_processor.h
      2. Check: Test-Path src/dlss/dlss_sr_processor.cpp
      3. Both should return True
    Expected Result: Both new files exist
    Failure Indicators: Either file missing
    Evidence: .sisyphus/evidence/task-3-sr-exists.txt
  ```

  **Commit**: YES (group 2)
  - Message: `refactor(dlss): replace RR with SR in ngx_wrapper and processor`
  - Files: `src/dlss/dlss_sr_processor.h`, `src/dlss/dlss_sr_processor.cpp`, (delete) `src/dlss/dlss_rr_processor.h`, `src/dlss/dlss_rr_processor.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [x] 4. GUI Types & State: Add preset to DlssConfig and config-store

  **What to do**:
  - In `gui/src/types/dlss-config.ts`:
    - Add `preset: 'J' | 'K' | 'L' | 'M';` to `DlssConfig` interface (after `quality` field)
    - Add `preset: 'L'` to `DEFAULT_CONFIG` (after `quality: 'MaxQuality'`)
  - In `gui/src/state/config-store.ts`:
    - Add `| { type: 'SET_PRESET'; payload: DlssConfig['preset'] }` to `ConfigAction` union (after the SET_QUALITY line 13)
    - Add reducer case: `case 'SET_PRESET': return { ...state, preset: action.payload };` (after SET_QUALITY case at line 67)

  **Must NOT do**:
  - Do not modify any existing action types or reducer cases
  - Do not change DEFAULT_CONFIG's existing field values
  - Do not add UI components (that's Task 6)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Two TypeScript files, adding one field and one action — trivial
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: No browser testing needed for type changes
    - `visual-engineering`: No UI changes

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 9)
  - **Blocks**: Task 6
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `gui/src/types/dlss-config.ts:6` — `quality` type union pattern (follow same for `preset`)
  - `gui/src/types/dlss-config.ts:23-43` — `DEFAULT_CONFIG` object (add `preset: 'L'` entry)
  - `gui/src/state/config-store.ts:13` — `SET_QUALITY` action pattern (follow same for `SET_PRESET`)
  - `gui/src/state/config-store.ts:66-67` — `SET_QUALITY` reducer case (follow same for `SET_PRESET`)

  **WHY Each Reference Matters**:
  - `quality` type — preset follows exact same union type pattern for consistency
  - `DEFAULT_CONFIG` — the default preset value 'L' must match user's explicit requirement
  - `SET_QUALITY` action/reducer — `SET_PRESET` is structurally identical, just different field

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: TypeScript compiles with new preset field
    Tool: Bash
    Preconditions: None
    Steps:
      1. Run: npx tsc --noEmit (in gui/ directory)
      2. Check exit code is 0
    Expected Result: Zero TypeScript errors
    Failure Indicators: Type errors referencing preset
    Evidence: .sisyphus/evidence/task-4-tsc-check.txt

  Scenario: DEFAULT_CONFIG includes preset: 'L'
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "gui/src/types/dlss-config.ts" -Pattern "preset: 'L'"
      2. Verify at least one match
    Expected Result: "preset: 'L'" found in DEFAULT_CONFIG
    Failure Indicators: No match
    Evidence: .sisyphus/evidence/task-4-default-preset.txt
  ```

  **Commit**: YES (group 4)
  - Message: `feat(gui): add SR preset selector and update config types`
  - Files: `gui/src/types/dlss-config.ts`, `gui/src/state/config-store.ts`
  - Pre-commit: `npx tsc --noEmit` in gui/

- [x] 5. sequence_processor.cpp: Update all 3 pipelines from RR to SR

  **What to do**:

  This is the largest single task — 1954-line file with 3 pipeline functions that all need RR→SR migration. Work through each function methodically:

  **A) Header and Guard updates (top of file)**:
  - Line 10: Change `#include "dlss/dlss_rr_processor.h"` → `#include "dlss/dlss_sr_processor.h"`
  - Lines 56-64: `DlssFeatureGuard` destructor — change `ngx.releaseDlssRR()` → `ngx.releaseDlssSR()`

  **B) sequence_processor.h updates**:
  - Line 37-40: Rename `processDirectoryRRFG` → `processDirectorySRFG` in the private declaration

  **C) processDirectory() (line 587-855) — SR-only pipeline**:
  - Line 594: Change `processDirectoryRRFG(` → `processDirectorySRFG(`
  - Line 626: Change `DlssRRProcessor processor(m_ctx, m_ngx)` → `DlssSRProcessor processor(m_ctx, m_ngx)`
  - Lines 651-657: Change `m_ngx.createDlssRR(` → `m_ngx.createDlssSR(` and add `config.preset` parameter after `config.quality`:
    ```cpp
    !m_ngx.createDlssSR(expectedInputWidth,
                        expectedInputHeight,
                        expectedOutputWidth,
                        expectedOutputHeight,
                        config.quality,
                        config.preset,
                        createCmdBuf,
                        errorMsg)
    ```
  - Line 682: REMOVE `specular` texture handle declaration entirely (SR doesn't use specular separately)
  - Line 716: REMOVE `const std::vector<float> specularRgba = expandRgbToRgba(mappedBuffers.specularAlbedo, 1.0f);`
  - Line 725: REMOVE `specular = texturePool->acquire(...)` line for specular texture
  - Line 736: REMOVE `texturePool->updateData(specular, specularRgba.data());`
  - Lines 741-764: Change `DlssFrameInput frame{}` → `DlssSRFrameInput frame{}` and REMOVE `frame.specularAlbedo` and `frame.specularView` fields (lines 750-751)
  - All other fields remain the same (color, depth, motionVectors, diffuseAlbedo, normals, roughness, output, dimensions, mvScale, reset)

  **D) processDirectoryRRFG() (line 857-1509) — Combined SR+FG pipeline — RENAME to processDirectorySRFG()**:
  - Line 857: Rename function `processDirectoryRRFG` → `processDirectorySRFG`
  - ALL `[RRFG]` log prefixes → `[SRFG]` (approximately 35+ occurrences through the function)
  - Line 862: `"[RRFG] Starting combined RR+FG pipeline"` → `"[SRFG] Starting combined SR+FG pipeline"`
  - Lines 951-958: Change `isDlssRRAvailable()` → `isDlssSRAvailable()`, update error message:
    ```cpp
    if (!m_ngx.isDlssSRAvailable()) {
        errorMsg = m_ngx.unavailableReason();
        if (errorMsg.empty()) {
            errorMsg = "DLSS Super Resolution is not supported on this GPU.";
        }
        ...
    }
    ```
  - Line 959: `"[RRFG] DLSS-RR available"` → `"[SRFG] DLSS-SR available"`
  - Lines 989: `DlssFeatureGuard featureGuardRR(m_ngx)` — keep name or rename to `featureGuardSR` for clarity
  - Lines 992-1016: Change `createDlssRR(` → `createDlssSR(` with added `config.preset` parameter. Update all log strings from "RR" to "SR":
    ```cpp
    std::fprintf(stdout, "[SRFG] Creating DLSS-SR feature...\n");
    ...
    !m_ngx.createDlssSR(expectedInputWidth,
                        expectedInputHeight,
                        expectedOutputWidth,
                        expectedOutputHeight,
                        config.quality,
                        config.preset,
                        createCmdBufRR,  // variable name can stay or rename
                        errorMsg)
    ...
    std::fprintf(stdout, "[SRFG] DLSS-SR feature created\n");
    ```
  - Line 1044: Change `DlssRRProcessor rrProcessor(m_ctx, m_ngx)` → `DlssSRProcessor srProcessor(m_ctx, m_ngx)`
  - Lines 1091-1093: REMOVE specular-related lines:
    - REMOVE `const std::vector<float> firstSpecularRgba = expandRgbToRgba(firstMappedBuffers.specularAlbedo, 1.0f);`
  - Line 1094: Rename `rrOutputInit` → `srOutputInit` (cosmetic, optional but clearer)
  - Lines 1103: REMOVE `TextureHandle firstSpecular;`
  - Line 1113: REMOVE `firstSpecular = pool.acquire(...)` line
  - Line 1122: REMOVE `pool.updateData(firstSpecular, firstSpecularRgba.data());`
  - Lines 1131-1154: Change `DlssFrameInput firstFrame{}` → `DlssSRFrameInput firstFrame{}`, REMOVE `firstFrame.specularAlbedo` and `firstFrame.specularView` (lines 1140-1141)
  - Line 1169: Change `rrProcessor.evaluate(` → `srProcessor.evaluate(`
  - Lines 1186+: Update log strings from "RR" to "SR"
  - Lines 1236: REMOVE `const std::vector<float> specularRgba = expandRgbToRgba(mappedBuffers.specularAlbedo, 1.0f);`
  - Lines 1243: REMOVE `TextureHandle specular;`
  - Line 1252: REMOVE `specular = firstSpecular;`
  - Line 1262: REMOVE `pool.updateData(specular, specularRgba.data());`
  - Lines 1272-1295: Change `DlssFrameInput rrFrame{}` → `DlssSRFrameInput srFrame{}`, REMOVE specular fields (lines 1281-1282), rename variable from `rrFrame` to `srFrame`
  - Line 1310: Change `rrProcessor.evaluate(rrEvalCmdBuf, rrFrame, errorMsg)` → `srProcessor.evaluate(rrEvalCmdBuf, srFrame, errorMsg)`
  - Line 1246: Rename `rrOutput` → `srOutput` (TextureHandle variable, used extensively). This is cosmetic but reduces confusion — if too many touch points, can keep as-is
  - ALL remaining `[RRFG]` log strings in the FG loop (lines 1311, 1319, 1412, 1453, 1479, 1484, 1489) → `[SRFG]`
  - **CRITICAL**: Do NOT touch any FG-related code (DlssFgProcessor, DlssFgFrameInput, fgProcessor.evaluate, PQ transport, LUT processing). Only change RR→SR references and log prefixes.

  **E) processDirectoryFG() (line 1511-1954) — FG-only pipeline**:
  - This function does NOT use RR/SR at all. No DlssRRProcessor, no createDlssRR. Only FG.
  - The ONLY changes here are cosmetic log string updates if any mention "RR" — scan and replace.
  - Based on reading, FG-only uses `[FG]` prefixes throughout and does NOT reference RR, so this function should need **zero changes**. Verify by scanning for any "RR" string references.

  **Must NOT do**:
  - Do not modify ANY FG logic (DlssFgProcessor, DlssFgFrameInput, createDlssFG, releaseDlssFG, fgProcessor.evaluate)
  - Do not modify PQ transport encoding/decoding logic
  - Do not modify LUT processing logic
  - Do not modify FramePrefetcher, ChannelMapper, MvConverter logic
  - Do not modify ExrReader/ExrWriter logic
  - Do not modify TexturePool logic
  - Do not modify AsyncExrWriter logic
  - Do not remove diffuse/normals/roughness textures — only specular is removed

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: Largest file in the project (1954 lines), 3 functions to update, requires careful identification of RR-specific vs FG-specific code to avoid contaminating FG logic
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not UI work

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Task 2 and 3 completing)
  - **Parallel Group**: Wave 3 (with Tasks 7, 10)
  - **Blocks**: Tasks 8, 10
  - **Blocked By**: Tasks 2, 3

  **References**:

  **Pattern References**:
  - `src/pipeline/sequence_processor.cpp:10` — `#include "dlss/dlss_rr_processor.h"` (change to sr)
  - `src/pipeline/sequence_processor.cpp:56-64` — `DlssFeatureGuard` destructor (change releaseDlssRR → releaseDlssSR)
  - `src/pipeline/sequence_processor.cpp:587-855` — `processDirectory()` full function (SR-only pipeline)
  - `src/pipeline/sequence_processor.cpp:626` — `DlssRRProcessor` instantiation (→ DlssSRProcessor)
  - `src/pipeline/sequence_processor.cpp:651-657` — `createDlssRR()` call (→ createDlssSR with preset)
  - `src/pipeline/sequence_processor.cpp:682` — `specular` texture handle (REMOVE)
  - `src/pipeline/sequence_processor.cpp:716` — specularRgba (REMOVE)
  - `src/pipeline/sequence_processor.cpp:725` — specular pool.acquire (REMOVE)
  - `src/pipeline/sequence_processor.cpp:736` — specular updateData (REMOVE)
  - `src/pipeline/sequence_processor.cpp:741-764` — DlssFrameInput struct population (→ DlssSRFrameInput, remove specular)
  - `src/pipeline/sequence_processor.cpp:857-1509` — `processDirectoryRRFG()` full function (→ processDirectorySRFG)
  - `src/pipeline/sequence_processor.cpp:951-958` — `isDlssRRAvailable()` check (→ isDlssSRAvailable)
  - `src/pipeline/sequence_processor.cpp:1000-1006` — `createDlssRR()` call in RRFG (→ createDlssSR with preset)
  - `src/pipeline/sequence_processor.cpp:1044` — `DlssRRProcessor rrProcessor` (→ DlssSRProcessor srProcessor)
  - `src/pipeline/sequence_processor.cpp:1092` — specularRgba in RRFG first frame (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1103` — firstSpecular handle (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1113` — firstSpecular acquire (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1122` — firstSpecular updateData (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1131-1154` — DlssFrameInput firstFrame (→ DlssSRFrameInput, remove specular)
  - `src/pipeline/sequence_processor.cpp:1169` — rrProcessor.evaluate (→ srProcessor.evaluate)
  - `src/pipeline/sequence_processor.cpp:1236` — specularRgba in loop (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1243` — specular handle in loop (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1252` — specular = firstSpecular (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1262` — specular updateData in loop (REMOVE)
  - `src/pipeline/sequence_processor.cpp:1272-1295` — DlssFrameInput rrFrame (→ DlssSRFrameInput srFrame, remove specular)
  - `src/pipeline/sequence_processor.cpp:1310` — rrProcessor.evaluate in loop (→ srProcessor.evaluate)
  - `src/pipeline/sequence_processor.cpp:1511-1954` — `processDirectoryFG()` (verify zero RR references, likely no changes)
  - `src/pipeline/sequence_processor.h:37-40` — `processDirectoryRRFG` declaration (→ processDirectorySRFG)

  **API/Type References**:
  - `src/dlss/dlss_sr_processor.h` — `DlssSRFrameInput` struct (created in Task 3) — this replaces `DlssFrameInput`; no specularAlbedo/specularView fields
  - `src/dlss/ngx_wrapper.h` — `createDlssSR()` signature (created in Task 2) — takes additional `DlssSRPreset preset` parameter vs old `createDlssRR()`

  **WHY Each Reference Matters**:
  - Every RR-specific reference is listed with its line number to enable precise surgical edits
  - Specular removal lines are explicitly called out because they're scattered across the file — missing one causes compile errors
  - FG "do not touch" boundary is critical because the RRFG function interleaves RR and FG calls — only the RR half changes
  - The `processDirectoryFG` section is listed to confirm it needs zero changes — prevents unnecessary edits

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Build succeeds after pipeline migration
    Tool: Bash
    Preconditions: Tasks 1, 2, 3 completed
    Steps:
      1. Run: cmake --build build --config Release
      2. Check exit code is 0
    Expected Result: Build succeeds with zero errors
    Failure Indicators: Compile errors in sequence_processor.cpp (undefined DlssFrameInput, specular references, missing createDlssSR args)
    Evidence: .sisyphus/evidence/task-5-build-success.txt

  Scenario: Zero RR references remain in sequence_processor files
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "src/pipeline/sequence_processor.*" -Pattern "DlssRR|DLSSD|RayReconstruction|dlss_rr|isDlssRRAvailable|createDlssRR|releaseDlssRR|\[RRFG\]"
      2. Verify zero matches
    Expected Result: Zero matches
    Failure Indicators: Any match found — means an RR reference was missed
    Evidence: .sisyphus/evidence/task-5-no-rr-refs.txt

  Scenario: FG functions and PQ transport untouched
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "src/pipeline/sequence_processor.cpp" -Pattern "DlssFgProcessor|createDlssFG|releaseDlssFG|fgProcessor\.evaluate|PqTransferProcessor|pqTransfer\.apply"
      2. Verify these references still exist (they should NOT have been removed)
      3. Verify processDirectoryFG function exists unchanged
    Expected Result: All FG and PQ references still present
    Failure Indicators: Missing FG references means FG code was accidentally modified
    Evidence: .sisyphus/evidence/task-5-fg-untouched.txt

  Scenario: No specular texture references remain
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "src/pipeline/sequence_processor.cpp" -Pattern "specular|specularAlbedo|specularRgba|specularView" -CaseSensitive
      2. Verify zero matches
    Expected Result: All specular references removed from this file
    Failure Indicators: Any specular reference remaining
    Evidence: .sisyphus/evidence/task-5-no-specular.txt

  Scenario: processDirectorySRFG function exists (renamed from RRFG)
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "src/pipeline/sequence_processor.cpp" -Pattern "processDirectorySRFG"
      2. Run: Select-String -Path "src/pipeline/sequence_processor.h" -Pattern "processDirectorySRFG"
      3. Verify matches in both files
      4. Run: Select-String -Path "src/pipeline/sequence_processor.*" -Pattern "processDirectoryRRFG"
      5. Verify zero matches (old name gone)
    Expected Result: SRFG exists in both .cpp and .h, RRFG exists in neither
    Failure Indicators: Old RRFG name still present, or SRFG missing
    Evidence: .sisyphus/evidence/task-5-srfg-renamed.txt
  ```

  **Commit**: YES (group 3)
  - Message: `refactor(pipeline): update sequence processor pipelines RR→SR`
  - Files: `src/pipeline/sequence_processor.cpp`, `src/pipeline/sequence_processor.h`
  - Pre-commit: `cmake --build build --config Release`

- [x] 6. GUI: Add preset dropdown to BasicSettings and wire --preset in cli-args

  **What to do**:
  - In `gui/src/components/BasicSettings.tsx`:
    - Add a preset `<select>` dropdown AFTER the quality dropdown (after line ~86-87, inside the same `{scaleEnabled && (...)}` conditional block)
    - Options: `J`, `K`, `L`, `M` with labels "Preset J", "Preset K", "Preset L (Default)", "Preset M"
    - Value bound to `config.preset`
    - onChange dispatches `{ type: 'SET_PRESET', payload: e.target.value as DlssConfig['preset'] }`
    - Show only when `scaleEnabled` is true (same visibility as quality dropdown)
    - Follow the exact same `<label>` + `<select>` + `<option>` pattern as the quality dropdown at lines 76-87
  - In `gui/electron/cli-args.ts`:
    - Add `args.push('--preset', config.preset)` when `scaleEnabled` is true
    - Insert after the quality push at line 12, inside the same `if (config.scaleEnabled)` block
  - In `gui/electron/progress-parser.ts`:
    - If any regex patterns match `[RRFG]` prefix, change to `[SRFG]`
    - Check line 11-12 for `\[RRFG\]` or `\[RR\]` patterns. Based on file reading (42 lines), it uses generic patterns — likely no changes needed, but verify.

  **Must NOT do**:
  - Do not modify the quality dropdown or any existing dropdown behavior
  - Do not add validation beyond what the type system provides (DlssConfig type enforces valid values)
  - Do not change layout/styling of existing components

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Frontend UI component work, adding a dropdown to a React settings panel
  - **Skills**: [`playwright`]
    - `playwright`: Needed for e2e verification of the dropdown rendering and interaction
  - **Skills Evaluated but Omitted**:
    - `frontend-ui-ux`: Overkill — this is a single `<select>` element following an existing pattern

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 2)
  - **Parallel Group**: Wave 2 (with Tasks 2, 3)
  - **Blocks**: Task 8
  - **Blocked By**: Task 4

  **References**:

  **Pattern References**:
  - `gui/src/components/BasicSettings.tsx:76-87` — Quality dropdown pattern (copy exactly for preset)
  - `gui/src/components/BasicSettings.tsx:62-63` — `scaleEnabled` conditional block structure
  - `gui/electron/cli-args.ts:10-12` — Quality arg push pattern (follow for preset)
  - `gui/electron/progress-parser.ts:1-42` — Full file (check for any RR references in regex patterns)

  **API/Type References**:
  - `gui/src/types/dlss-config.ts` — `DlssConfig` interface with `preset` field (from Task 4)
  - `gui/src/state/config-store.ts` — `SET_PRESET` action (from Task 4)

  **WHY Each Reference Matters**:
  - Quality dropdown — preset dropdown is structurally identical, just different options and action type
  - cli-args.ts quality push — preset push follows exact same pattern inside same conditional
  - progress-parser — verify no RR log prefix patterns exist that would break parsing

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Preset dropdown renders when upscaling enabled
    Tool: Playwright (via playwright skill)
    Preconditions: GUI dev server running (`npm run dev` in gui/)
    Steps:
      1. Navigate to http://localhost:5173 (or dev port)
      2. Check the "Enable Upscaling" toggle if not already on
      3. Wait for the preset dropdown to appear: selector 'select' with options J, K, L, M
      4. Verify default selected value is "L"
      5. Take screenshot
    Expected Result: Preset dropdown visible, default "L" selected
    Failure Indicators: Dropdown missing, wrong default, not visible when scale enabled
    Evidence: .sisyphus/evidence/task-6-preset-dropdown.png

  Scenario: Preset dropdown hidden when upscaling disabled
    Tool: Playwright (via playwright skill)
    Preconditions: GUI dev server running
    Steps:
      1. Navigate to http://localhost:5173
      2. Uncheck "Enable Upscaling" toggle
      3. Verify preset dropdown is NOT visible in the DOM or is hidden
    Expected Result: Preset dropdown not rendered
    Failure Indicators: Dropdown still visible when scale disabled
    Evidence: .sisyphus/evidence/task-6-preset-hidden.png

  Scenario: CLI args include --preset when upscaling enabled
    Tool: Bash
    Preconditions: Task 4 types completed
    Steps:
      1. Run: Select-String -Path "gui/electron/cli-args.ts" -Pattern "--preset"
      2. Verify at least one match inside the scaleEnabled block
    Expected Result: --preset arg construction present
    Failure Indicators: --preset not found in cli-args.ts
    Evidence: .sisyphus/evidence/task-6-cli-args-preset.txt

  Scenario: TypeScript compiles cleanly
    Tool: Bash
    Preconditions: Tasks 4 and 6 complete
    Steps:
      1. Run: npx tsc --noEmit (in gui/ directory)
      2. Check exit code is 0
    Expected Result: Zero TypeScript errors
    Failure Indicators: Type errors related to preset
    Evidence: .sisyphus/evidence/task-6-tsc-check.txt
  ```

  **Commit**: YES (group 4)
  - Message: `feat(gui): add SR preset selector and update config types`
  - Files: `gui/src/components/BasicSettings.tsx`, `gui/electron/cli-args.ts`, `gui/electron/progress-parser.ts` (if changed)
  - Pre-commit: `npx tsc --noEmit` in gui/

- [x] 7. CLI help text + stdout log strings + residual RR reference cleanup

  **What to do**:
  - In `src/cli/cli_parser.cpp`:
    - Line 429: `--test-ngx` help text — already addressed in Task 1 (verify it says "DLSS-SR" not "DLSS-RR")
    - Scan ALL strings in the file for "RR", "Ray Reconstruction", "DLSSD" references and update to SR equivalents
  - In `src/pipeline/sequence_processor.cpp`:
    - Verify ALL log strings updated by Task 5 — this task is a secondary sweep
    - Scan for any remaining `"RR"` or `"Ray Reconstruction"` in string literals
  - In `src/dlss/ngx_wrapper.cpp`:
    - Verify ALL error/log strings updated by Task 2
    - Scan for any remaining `"RR"` or `"Ray Reconstruction"` references
  - **Global scan across all src/ files**:
    - Run: `Select-String -Path "src/**/*" -Pattern "Ray Reconstruction|DLSS-RR|DlssRR|dlss_rr|DLSSD" -Recurse`
    - Fix any remaining hits (excluding DLSS/ SDK headers which are read-only)
  - In `gui/` directory:
    - Run: `Select-String -Path "gui/src/**/*" -Pattern "Ray Reconstruction|DLSS-RR|dlss_rr" -Recurse`
    - Check `gui/src/components/BasicSettings.tsx` — line 28 tooltip or label might say "Ray Reconstruction"
    - Check `gui/src/types/dlss-config.ts` — any comments mentioning RR
    - Fix any remaining hits

  **Must NOT do**:
  - Do not modify files in `DLSS/include/` (SDK headers are read-only)
  - Do not change any logic — this is purely string/comment cleanup
  - Do not change file names or add new files

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Search-and-replace of string literals across already-modified files
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not UI work

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 3, alongside Tasks 5, 10)
  - **Parallel Group**: Wave 3 (with Tasks 5, 10)
  - **Blocks**: Task 8
  - **Blocked By**: Task 2

  **References**:

  **Pattern References**:
  - `src/cli/cli_parser.cpp:408-436` — `printHelp()` function (scan for any remaining RR strings)
  - `src/pipeline/sequence_processor.cpp` — full file (secondary sweep after Task 5)
  - `src/dlss/ngx_wrapper.cpp` — full file (secondary sweep after Task 2)
  - `gui/src/components/BasicSettings.tsx` — labels/tooltips (check for RR mentions)
  - `gui/src/types/dlss-config.ts` — comments (check for RR mentions)

  **WHY Each Reference Matters**:
  - This task is the "safety net" — it catches anything Tasks 1-5 might have missed
  - A single remaining "Ray Reconstruction" in a user-visible string breaks the user experience

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Zero RR references in entire src/ tree (excluding SDK)
    Tool: Bash
    Preconditions: Tasks 1-5 completed
    Steps:
      1. Run: Select-String -Path "src" -Pattern "Ray Reconstruction|DLSS-RR|DlssRR|dlss_rr|SuperSamplingDenoising" -Recurse -Include "*.h","*.cpp"
      2. Verify zero matches
    Expected Result: Zero matches across all source files
    Failure Indicators: Any match — residual RR reference found
    Evidence: .sisyphus/evidence/task-7-src-clean.txt

  Scenario: Zero RR references in GUI source
    Tool: Bash
    Preconditions: Tasks 4, 6 completed
    Steps:
      1. Run: Select-String -Path "gui/src" -Pattern "Ray Reconstruction|DLSS-RR|dlss_rr" -Recurse -Include "*.ts","*.tsx"
      2. Verify zero matches
    Expected Result: Zero matches
    Failure Indicators: Any match in GUI source
    Evidence: .sisyphus/evidence/task-7-gui-clean.txt

  Scenario: --help output contains no RR references
    Tool: Bash
    Preconditions: Build completed
    Steps:
      1. Run: ./build/Release/dlss-compositor.exe --help 2>&1
      2. Pipe output through Select-String for "RR|Ray Reconstruction"
      3. Verify zero matches
      4. Verify output contains "SR" or "Super Resolution"
    Expected Result: Help text fully migrated to SR terminology
    Failure Indicators: "RR" or "Ray Reconstruction" found in help output
    Evidence: .sisyphus/evidence/task-7-help-clean.txt
  ```

  **Commit**: YES (group 3)
  - Message: `refactor(pipeline): update sequence processor pipelines RR→SR`
  - Files: Any files with residual RR references found during scan
  - Pre-commit: `cmake --build build --config Release`

- [x] 8. Build verification + full codebase audit for RR residuals

  **What to do**:
  - **Step 1 — Full Release Build**:
    - Run: `cmake --build build --config Release`
    - Must succeed with zero errors and zero warnings related to DLSS/NGX
  - **Step 2 — Codebase-wide RR reference audit**:
    - Run: `Select-String -Path "src" -Pattern "RayReconstruction|DLSSD|dlss_rr|DlssRR|isDlssRRAvailable|createDlssRR|releaseDlssRR|SuperSamplingDenoising|specularAlbedo" -Recurse -Include "*.h","*.cpp"`
    - Expected: zero matches
    - Run: `Select-String -Path "gui/src" -Pattern "RayReconstruction|DLSS-RR|dlss_rr|DlssRR" -Recurse -Include "*.ts","*.tsx"`
    - Expected: zero matches
    - NOTE: `DLSS/include/` SDK headers will still have these — they are READ-ONLY, do NOT count them
  - **Step 3 — CLI smoke test**:
    - Run: `./build/Release/dlss-compositor.exe --help`
    - Verify: output mentions `--preset`, mentions "SR" or "Super Resolution", does NOT mention "RR" or "Ray Reconstruction"
    - Run: `./build/Release/dlss-compositor.exe --preset X 2>&1`
    - Verify: fails with clear error about invalid preset value
  - **Step 4 — File existence check**:
    - Verify `src/dlss/dlss_sr_processor.h` EXISTS
    - Verify `src/dlss/dlss_sr_processor.cpp` EXISTS
    - Verify `src/dlss/dlss_rr_processor.h` does NOT exist
    - Verify `src/dlss/dlss_rr_processor.cpp` does NOT exist
  - **Step 5 — GUI build**:
    - Run `npm run build` in `gui/` directory
    - Must succeed with zero errors

  **Must NOT do**:
  - Do not make any code changes in this task — this is verification only
  - If issues are found, report them but do not fix (previous tasks should have fixed everything)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Multi-step verification across C++, TypeScript, and CLI — requires running builds and multiple scans
  - **Skills**: [`playwright`]
    - `playwright`: May be needed if GUI e2e test is included
  - **Skills Evaluated but Omitted**: None

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on ALL previous tasks)
  - **Parallel Group**: Wave 4 (solo)
  - **Blocks**: F1-F4
  - **Blocked By**: Tasks 5, 6, 7, 10

  **References**:

  **Pattern References**:
  - All files modified in Tasks 1-7 (see individual task references)

  **WHY Each Reference Matters**:
  - This is the integration verification task — validates that all individual task changes compose correctly into a working build

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Clean Release build
    Tool: Bash
    Preconditions: All Tasks 1-7 completed
    Steps:
      1. Run: cmake --build build --config Release 2>&1
      2. Check exit code is 0
      3. Save full build output
    Expected Result: Build succeeds, zero errors
    Failure Indicators: Any compile or link error
    Evidence: .sisyphus/evidence/task-8-release-build.txt

  Scenario: Zero RR residuals in source tree
    Tool: Bash
    Preconditions: All edits applied
    Steps:
      1. Run: Select-String -Path "src" -Pattern "RayReconstruction|DLSSD|dlss_rr|DlssRR|isDlssRRAvailable|createDlssRR|releaseDlssRR|SuperSamplingDenoising" -Recurse -Include "*.h","*.cpp"
      2. Verify zero matches
      3. Run: Select-String -Path "gui/src" -Pattern "RayReconstruction|DLSS-RR|dlss_rr|DlssRR" -Recurse -Include "*.ts","*.tsx"
      4. Verify zero matches
    Expected Result: Zero RR references in any source file
    Failure Indicators: Any match found
    Evidence: .sisyphus/evidence/task-8-rr-audit.txt

  Scenario: CLI --help shows SR terminology
    Tool: Bash
    Preconditions: Build completed
    Steps:
      1. Run: $output = ./build/Release/dlss-compositor.exe --help 2>&1
      2. Verify: $output contains "--preset"
      3. Verify: $output does NOT contain "Ray Reconstruction" or "DLSS-RR"
      4. Verify: $output contains "SR" or "Super Resolution"
    Expected Result: Help text fully migrated
    Failure Indicators: Any RR terminology in help output
    Evidence: .sisyphus/evidence/task-8-help-output.txt

  Scenario: Old RR processor files deleted, new SR files exist
    Tool: Bash
    Preconditions: Task 3 completed
    Steps:
      1. Verify: Test-Path src/dlss/dlss_sr_processor.h → True
      2. Verify: Test-Path src/dlss/dlss_sr_processor.cpp → True
      3. Verify: Test-Path src/dlss/dlss_rr_processor.h → False
      4. Verify: Test-Path src/dlss/dlss_rr_processor.cpp → False
    Expected Result: SR files exist, RR files deleted
    Failure Indicators: Any mismatch
    Evidence: .sisyphus/evidence/task-8-file-check.txt

  Scenario: GUI builds successfully
    Tool: Bash
    Preconditions: Tasks 4, 6 completed
    Steps:
      1. Run: npm run build (in gui/ directory)
      2. Check exit code is 0
    Expected Result: GUI build succeeds
    Failure Indicators: Build errors
    Evidence: .sisyphus/evidence/task-8-gui-build.txt
  ```

  **Commit**: NO (verification only — no code changes)

- [x] 9. Blender addon: Simplify AOV configuration for SR

  **What to do**:
  - In `blender/dlss_compositor_aov/__init__.py`:
    - **GlossyColor pass**: SR does not use specular albedo as a separate buffer. REMOVE `vl.use_pass_glossy_color = True` (line 116)
    - **Keep all other passes**: Combined (line 111), Z (112), Vector (113), Normal (114), DiffuseColor (115), Roughness AOV (118-123) — all stay
    - **File Output node pass_names** (lines 228-236): REMOVE the `("Glossy Color", "RGBA")` entry from the list
    - **Pass Status panel** (lines 332-335): REMOVE the `col.label(text=f"Glossy Color: ...")` block
    - **Report string** (line 259): Update from `"Combined, Z, Vector, Normal, DiffCol, GlossCol, Roughness(AOV)"` to `"Combined, Z, Vector, Normal, DiffCol, Roughness(AOV)"`
    - **Headless test print** (line 383-384): Update from `"Passes configured: Combined, Z, Vector, Normal, DiffCol, GlossCol, Roughness(AOV)"` to `"Passes configured: Combined, Z, Vector, Normal, DiffCol, Roughness(AOV)"`
    - **Roughness AOV wiring** (lines 125-189): Keep entirely as-is — Roughness is still an optional GBuffer hint for SR
    - **Note**: The addon does NOT wire specular data to the compositor — GlossyColor was only enabled as a pass for the old RR pipeline to read. With SR, specular is not used, so we simply stop enabling/exporting it.

  **Must NOT do**:
  - Do not remove Normal or DiffuseColor passes — these are used as optional GBuffer hints by SR
  - Do not remove Roughness AOV — it's an optional GBuffer hint
  - Do not modify the Camera Export operator
  - Do not modify the compositor wiring logic structure (just remove the one entry)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Single Python file, removing ~6 lines related to GlossyColor pass
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not a browser task

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 4)
  - **Blocks**: Task 10
  - **Blocked By**: None (can start immediately)

  **References**:

  **Pattern References**:
  - `blender/dlss_compositor_aov/__init__.py:111-116` — Built-in render passes (remove line 116: glossy_color)
  - `blender/dlss_compositor_aov/__init__.py:228-236` — `pass_names` list for File Output node (remove GlossyColor entry)
  - `blender/dlss_compositor_aov/__init__.py:332-335` — Glossy Color status label in panel (remove)
  - `blender/dlss_compositor_aov/__init__.py:259` — Report string (remove GlossCol)
  - `blender/dlss_compositor_aov/__init__.py:383-384` — Headless test print string (remove GlossCol)

  **WHY Each Reference Matters**:
  - Line 116 enables the pass in Blender — removing it stops Blender from rendering unnecessary specular data
  - pass_names list controls what gets wired to the EXR file output — removing GlossyColor stops it from being written
  - Panel status shows users which passes are active — removing GlossyColor avoids confusion
  - Report/print strings are user-visible confirmation messages

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: GlossyColor references removed from addon
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "blender/dlss_compositor_aov/__init__.py" -Pattern "glossy_color|Glossy Color|GlossCol"
      2. Verify zero matches
    Expected Result: All GlossyColor references removed
    Failure Indicators: Any glossy reference remaining
    Evidence: .sisyphus/evidence/task-9-no-glossy.txt

  Scenario: Required passes still present
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "blender/dlss_compositor_aov/__init__.py" -Pattern "use_pass_combined|use_pass_z|use_pass_vector|use_pass_normal|use_pass_diffuse_color"
      2. Verify 5 matches (one per required pass)
      3. Run: Select-String -Path "blender/dlss_compositor_aov/__init__.py" -Pattern "Roughness"
      4. Verify Roughness AOV logic still present
    Expected Result: All required passes still configured
    Failure Indicators: Any required pass missing
    Evidence: .sisyphus/evidence/task-9-passes-intact.txt

  Scenario: Python syntax valid
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: python -c "import ast; ast.parse(open('blender/dlss_compositor_aov/__init__.py').read())"
      2. Check exit code is 0
    Expected Result: Python file parses without syntax errors
    Failure Indicators: SyntaxError
    Evidence: .sisyphus/evidence/task-9-syntax-check.txt
  ```

  **Commit**: YES (group 5)
  - Message: `refactor(addon): simplify Blender AOV config for SR`
  - Files: `blender/dlss_compositor_aov/__init__.py`
  - Pre-commit: `python -c "import ast; ast.parse(open('blender/dlss_compositor_aov/__init__.py').read())"`

- [x] 10. Documentation: Update README, usage guide, input spec, and build guide

  **What to do**:

  **A) README.md** (top-level):
  - Line 1 description: Change "DLSS Ray Reconstruction (DLSS-RR) and DLSS Frame Generation" → "DLSS Super Resolution (DLSS-SR) and DLSS Frame Generation"
  - Line 4 summary: "DLSS-RR for denoising and upscaling" → "DLSS-SR for upscaling"
  - All "DLSS-RR" → "DLSS-SR"
  - All "Ray Reconstruction" → "Super Resolution"
  - Features list: "DLSS-RR denoising and upscaling" → "DLSS Super Resolution upscaling"
  - Features list: "Combined RR+FG pipeline" → "Combined SR+FG pipeline"
  - Features list: "Motion vector conversion (Blender 4-channel to DLSS-RR 2-channel" → "...to DLSS-SR 2-channel"
  - Requirements: "must support DLSS Ray Reconstruction" → "must support DLSS Super Resolution"
  - CLI examples: Add `--preset L` to relevant examples showing `--scale`
  - Combined mode section: "DLSS-RR upscales/denoises each frame first" → "DLSS-SR upscales each frame first"
  - License section: no change

  **B) docs/usage_guide.md**:
  - Line 20 header: "DLSS-RR (Ray Reconstruction & Upscaling)" → "DLSS-SR (Super Resolution & Upscaling)"
  - Line 21: "Toggle spatial upscaling and denoising" → "Toggle spatial upscaling"
  - Line 76 section header: "Combined Mode (RR + FG)" → "Combined Mode (SR + FG)"
  - Line 78: "DLSS-RR upscales/denoises" → "DLSS-SR upscales"
  - Line 86: "All requirements for both RR and FG apply" → "All requirements for both SR and FG apply"
  - Line 104: "temporal denoising and ray reconstruction" → "temporal super resolution"
  - Line 133: `--test-ngx`: "Verify DLSS Ray Reconstruction availability" → "Verify DLSS Super Resolution availability"
  - Line 143: "InReset=true" troubleshooting: "DLSS-RR relies on temporal history" → "DLSS-SR relies on temporal history"
  - Line 150: "Ray Reconstruction is a relatively new feature" → "Super Resolution requires modern drivers"
  - Options table: Add `--preset <mode>` row: `| \`--preset <mode>\` | L | SR quality preset hint: J, K, L, or M. |`
  - CLI examples: Add `--preset L` to relevant examples

  **C) docs/dlss_input_spec.md**:
  - Title: "DLSS Input Specification" — keep, but update "passed to DLSS-RR" → "passed to DLSS-SR"
  - Line 15: "Specular Albedo" row — ADD a note: "(Not used by SR — optional, may be omitted)" or REMOVE the row entirely since SR doesn't use it
  - Decision: REMOVE the Specular Albedo row from the buffer table (SR doesn't use specular)
  - Line 23: "Optional Buffers" section: Remove "Specular Albedo" default
  - Line 35: "DLSS-RR expects" → "DLSS-SR expects"
  - Line 50: "DLSS-RR is a temporal denoiser" → "DLSS-SR is a temporal upscaler"

  **D) docs/build_guide.md**:
  - Line 10: "DLSS Ray Reconstruction" → "DLSS Super Resolution"
  - Line 42: `--test-ngx` — "DLSS availability check" is generic enough, verify it doesn't say "RR"
  - Line 100-101: `nvngx_dlssd.dll` — this DLL is for RR. SR uses `nvngx_dlss.dll`. Update the troubleshooting text accordingly:
    - Change "nvngx_dlssd.dll not found" → "nvngx_dlss.dll not found" (if applicable)
    - Update the copy path from `DLSS\lib\Windows_x86_64\rel\nvngx_dlssd.dll` to correct SR DLL
    - **NOTE**: Verify which DLL SR actually needs by checking CMakeLists.txt — may need to update DLL copy step too
  - Line 103-104: "DLSS-RR not available" → "DLSS-SR not available", "DLSS Ray Reconstruction specifically requires" → "DLSS Super Resolution requires"

  **Must NOT do**:
  - Do not change FG-related documentation sections
  - Do not change PQ transport documentation
  - Do not add new documentation files
  - Do not rewrite sections that don't mention RR

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Pure documentation update across 4 markdown files
  - **Skills**: []
  - **Skills Evaluated but Omitted**:
    - `playwright`: Not UI work

  **Parallelization**:
  - **Can Run In Parallel**: YES (within Wave 3)
  - **Parallel Group**: Wave 3 (with Tasks 5, 7)
  - **Blocks**: Task 8
  - **Blocked By**: Tasks 5, 9

  **References**:

  **Pattern References**:
  - `README.md` — Full file (multiple RR→SR replacements)
  - `docs/usage_guide.md` — Full file (section headers, options table, troubleshooting)
  - `docs/dlss_input_spec.md` — Full file (buffer table, motion vector section)
  - `docs/build_guide.md` — Full file (prerequisites, troubleshooting DLL names)

  **WHY Each Reference Matters**:
  - All four docs files are user-facing — any remaining "Ray Reconstruction" breaks user confidence in the migration
  - The DLL name in build_guide.md is particularly important — wrong DLL name will cause users to copy the wrong file
  - The input spec needs the specular row removed to avoid users rendering unnecessary passes

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: Zero RR references in documentation
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "README.md","docs/usage_guide.md","docs/dlss_input_spec.md","docs/build_guide.md" -Pattern "Ray Reconstruction|DLSS-RR|dlss_rr"
      2. Verify zero matches
    Expected Result: All RR references replaced with SR equivalents
    Failure Indicators: Any match remaining
    Evidence: .sisyphus/evidence/task-10-docs-clean.txt

  Scenario: --preset documented in usage guide
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "docs/usage_guide.md" -Pattern "--preset"
      2. Verify at least one match (in options table)
    Expected Result: --preset option documented
    Failure Indicators: --preset not found in usage guide
    Evidence: .sisyphus/evidence/task-10-preset-documented.txt

  Scenario: Specular Albedo removed from input spec
    Tool: Bash
    Preconditions: Edits applied
    Steps:
      1. Run: Select-String -Path "docs/dlss_input_spec.md" -Pattern "Specular Albedo|GlossCol|specular"
      2. Verify zero matches
    Expected Result: No specular references in input spec
    Failure Indicators: Specular row still present
    Evidence: .sisyphus/evidence/task-10-no-specular.txt
  ```

  **Commit**: YES (group 6)
  - Message: `docs: update README and guides for SR migration`
  - Files: `README.md`, `docs/usage_guide.md`, `docs/dlss_input_spec.md`, `docs/build_guide.md`
  - Pre-commit: —

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in .sisyphus/evidence/. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `cmake --build build --config Release`. Review all changed files for: `as any`/`@ts-ignore` (TS), empty catches, console.log in prod, commented-out code, unused imports. Check AI slop: excessive comments, over-abstraction, generic names. Verify no `DLSSD` references remain (except in SDK headers under `DLSS/include/` which are read-only).
  Output: `Build [PASS/FAIL] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high` (+ `playwright` skill if UI)
  Start from clean state. Execute EVERY QA scenario from EVERY task. Test cross-task integration. Test: `--help` shows SR not RR, `--preset L` parses, GUI preset dropdown works, build succeeds. Save to `.sisyphus/evidence/final-qa/`.
  Output: `Scenarios [N/N pass] | Integration [N/N] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff. Verify 1:1 — everything in spec was built, nothing beyond spec was built. Check FG code untouched, PQ code untouched, EXR reader/writer untouched. Flag unaccounted changes.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | VERDICT`

---

## Commit Strategy

| Group | Tasks | Message | Pre-commit |
|-------|-------|---------|------------|
| 1 | T1 | `feat(config): add DlssSRPreset enum and --preset CLI option` | `cmake --build build --config Release` |
| 2 | T2, T3 | `refactor(dlss): replace RR with SR in ngx_wrapper and processor` | `cmake --build build --config Release` |
| 3 | T5, T7 | `refactor(pipeline): update sequence processor pipelines RR→SR` | `cmake --build build --config Release` |
| 4 | T4, T6 | `feat(gui): add SR preset selector and update config types` | `npm run build` in gui/ |
| 5 | T9 | `refactor(addon): simplify Blender AOV config for SR` | — |
| 6 | T10 | `docs: update README and guides for SR migration` | — |

---

## Success Criteria

### Verification Commands
```bash
cmake --build build --config Release  # Expected: BUILD_SUCCESS, 0 errors
./build/Release/dlss-compositor.exe --help  # Expected: shows --preset option, no RR references
grep -r "RayReconstruction\|DLSSD\|dlss_rr" src/ gui/  # Expected: 0 matches (exclude DLSS/include/)
```

### Final Checklist
- [ ] All "Must Have" present
- [ ] All "Must NOT Have" absent
- [ ] cmake Release build passes
- [ ] `--preset` CLI option documented and parseable
- [ ] GUI preset selector visible
- [ ] FG logic completely untouched
- [ ] PQ transport completely untouched
