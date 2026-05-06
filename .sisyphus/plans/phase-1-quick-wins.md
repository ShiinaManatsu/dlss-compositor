# Phase 1 — Quick Wins (Low-Risk, High-Value Fixes)

## TL;DR

> **目标**: 一组小而高 ROI 的修复——jitter 算法 + 帧时刻 + version 常量合并 + CLI custom tonemap 解析 + NGX log 级别可配。每项独立、低风险、新增 flag 受控。
>
> **交付物**:
> - Blender Halton jitter 按缩放比平方计算相位数（修真实 SR 质量问题）
> - Jitter 在渲染回调里计算并写入 camera.json（消除算法漂移风险）
> - DLSS `InFrameTimeDeltaInMsec` 从 CLI `--source-fps` 派生
> - 合并 `kNgxEngineVersion` 双份常量到 `core/version.h`
> - CLI `--tonemap custom` 解析（之前完全不识别）
> - NGX log 级别可配，默认 WARN（之前 VERBOSE 太吵）
>
> **预估工作量**: M（多个独立小项）
> **并行执行**: YES — 几乎全部独立
> **关键路径**: 无强依赖，可任意顺序

**前置**: Phase 0 必须先完成（C++ GUI 已删除）

---

## Context

### Original Request
今日讨论中识别出多个小而独立的修复点，单独做都简单，但组合起来对 SR 质量、维护性、用户体验都有显著提升。

### Background
- **Jitter 相位**: 当前硬编码 8，但 nvpro-samples/vk_streamline 公式是 `8 × scaleRatio²`。2x 缩放下应是 32 相位 → 我们只覆盖 1/4 像素位置 → 时域积累质量打折。
- **Jitter 算法漂移**: Render handler 和 export_camera operator 各自独立计算 Halton。理论一致但风险存在。
- **Frame time delta**: 硬编码 33.3f（30fps）。24fps 用户偏差 28%，影响 DLSS 时域启发式。
- **Version 双份**: `ngx_wrapper.cpp:32` 是 "0.2.1"，`vulkan_context.cpp:30` 是 "0.1.0"——不一致。
- **CLI `--tonemap custom`**: README 宣传支持，但 parser 不识别"custom"字符串 → silently fall back 到 PQ。
- **NGX log VERBOSE**: 生产模式刷屏，影响日志质量。

### 已确认的设计
- `--source-fps` 是新 flag（不冲突 `--fps`，后者用于视频编码）
- `--ngx-log-level` 默认 WARN（之前 VERBOSE）
- Jitter 写入 camera.json 由渲染回调主导，export_camera 读取（避免双算）

---

## Work Objectives

### Core Objective
通过一组独立的、低风险的修复消除当前已识别的小问题，为后续 Phase 2/3 的大改动铺平地基。

### Concrete Deliverables
- 修改后的 `blender/dlss_compositor_aov/__init__.py` 和 `blender/aov_export_preset.py`
- 新增的 `src/core/version.h`
- 修改后的 `src/dlss/ngx_wrapper.cpp`、`src/gpu/vulkan_context.cpp`
- 修改后的 `src/dlss/dlss_sr_processor.cpp/h`、`src/pipeline/sequence_processor.cpp`
- 修改后的 `src/cli/cli_parser.cpp`、`src/cli/config.h`
- 更新的测试

### Definition of Done
- [ ] Blender 渲染：camera.json 含正确 `jitter_phases` 字段，jitter 值等于 `apply_jitter` 回调实际施加的值
- [ ] `--source-fps 24` CLI 调用：日志显示 frame time delta = 41.67ms
- [ ] `dlss-compositor.exe --version` 显示统一版本字符串
- [ ] `--tonemap custom --tonemap-lut foo.bin --inverse-tonemap-lut bar.bin` 不再报错
- [ ] `--ngx-log-level off` 时无 NGX 日志输出
- [ ] 全部测试通过

### Must Have
- 6 个独立修复全部 commit
- 每个 commit 独立可回退
- 所有现有测试不退化

### Must NOT Have (Guardrails)
- ❌ 不改 SR/FG 核心 eval 逻辑（GBuffer 删除留 Phase 2）
- ❌ 不改 sharpen / preset 默认值（Phase 2）
- ❌ 不改任何 EXR 读写
- ❌ 不引入新依赖（无新 npm/pip/CMake 包）
- ❌ 不重构 CLI 整体结构
- ❌ 不修改 Electron GUI（除非新 CLI flag 需 GUI 暴露——本 phase 不必）

---

## Verification Strategy

### Test Decision
- **基础设施**: YES (Catch2, pytest)
- **自动化测试**: YES (Tests-after，新增/更新测试)
- **框架**: 现有

### QA Policy
- Blender 改动用 headless 渲染验证：`blender --background --python ...`
- C++ 改动用 ctest 验证
- CLI 改动用 `--help` + 实际调用验证
- Jitter 算法一致性：渲染 8 帧后检查 camera.json 里的值与 `apply_jitter` 应用的值完全一致

证据保存到 `.sisyphus/evidence/phase1/task-{N}-{slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (全部并行 — 任务间无强依赖):
├── Task 1: 合并 kNgxEngineVersion (XS)
├── Task 2: CLI 解析 --tonemap custom (XS)
├── Task 3: NGX log 级别可配 (S)
├── Task 4: --source-fps + frame time delta (S)
├── Task 5: Jitter 算法统一（回调写 camera.json）(S)
└── Task 6: Jitter 相位按 scaleRatio² (S，需要 Task 5 先做完更安全)

Wave FINAL (审查):
├── F1: 计划合规审计
├── F2: 代码质量审查
├── F3: 真实 QA
└── F4: 范围保真度
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | — | 1 |
| 2 | — | — | 1 |
| 3 | — | — | 1 |
| 4 | — | — | 1 |
| 5 | — | 6 | 1 |
| 6 | 5 | — | 1 |

### Agent Dispatch Summary

- **Wave 1**: 6 tasks — T1-T2 → `quick`, T3-T6 → `quick`
- **FINAL**: 4 tasks — F1 → `oracle`, F2-F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [ ] 1. 合并 kNgxEngineVersion 双份常量

  **What to do**:
  - 新建 `src/core/version.h`:
    ```cpp
    #pragma once
    namespace appver {
        constexpr const char* kEngineName = "dlss-compositor";
        constexpr const char* kEngineVersion = "0.2.1";
        constexpr const char* kNgxProjectId = "6c6f53ec-6f25-4f9f-8d71-2f0f3c5e7a11";
        constexpr const char* kDlssSdkVersion = "310.5.3";
    }
    ```
  - 修改 `src/dlss/ngx_wrapper.cpp:32`：删除 `constexpr char kNgxEngineVersion[] = "0.2.1";`，include `core/version.h` 并使用 `appver::kEngineVersion`
  - 修改 `src/gpu/vulkan_context.cpp:30`：删除 `constexpr char kNgxEngineVersion[] = "0.1.0";`，使用 `appver::kEngineVersion`
  - 检查所有 `kNgxProjectId` 引用，统一到 `appver::kNgxProjectId`
  - 更新 `--version` 输出（如有）也从 `appver::` 读取

  **Must NOT do**:
  - 不改 NGX init 调用语义
  - 不引入新版本号

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None (Phase 0 完成后)

  **References**:
  - `src/dlss/ngx_wrapper.cpp:32`
  - `src/gpu/vulkan_context.cpp:30`

  **Acceptance Criteria**:
  - [ ] `src/core/version.h` 存在
  - [ ] `grep "kNgxEngineVersion" src/` 仅在 version.h 出现
  - [ ] Build 通过

  **QA Scenarios**:
  ```
  Scenario: 单一来源
    Tool: Grep
    Steps:
      1. Select-String -Path "src\**\*.cpp","src\**\*.h" -Pattern "EngineVersion\s*=\s*`"" -SimpleMatch:$false
      2. 应仅在 src/core/version.h 中匹配 1 次
    Expected Result: 1 match in version.h only
    Evidence: .sisyphus/evidence/phase1/task-1-single-source.txt
  ```

  **Commit**: YES
  - Message: `refactor(version): consolidate engine version into core/version.h`
  - Files: `src/core/version.h, src/dlss/ngx_wrapper.cpp, src/gpu/vulkan_context.cpp`
  - Pre-commit: `cmake --build build --config Release`

- [ ] 2. CLI 支持 --tonemap custom

  **What to do**:
  - 修改 `src/cli/cli_parser.cpp` 中的 `parseTonemapMode` 函数，新增分支：
    ```cpp
    if (std::strcmp(value, "custom") == 0) { out = TonemapMode::Custom; return true; }
    ```
  - 验证 `TonemapMode::Custom` 已在 `src/cli/config.h` 定义（如未定义需加）
  - 新增/更新 `tests/test_cli.cpp` 中的 tonemap 解析测试
  - 验证文档（README）中关于 `--tonemap custom` 的描述准确

  **Must NOT do**:
  - 不实现 Custom LUT 加载（已存在）
  - 不改其他 tonemap 模式

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)

  **References**:
  - `src/cli/cli_parser.cpp` parseTonemapMode (~line 110-120)
  - `src/cli/config.h` TonemapMode enum

  **Acceptance Criteria**:
  - [ ] `dlss-compositor.exe --tonemap custom --tonemap-lut foo.bin` 不报 "unknown tonemap"
  - [ ] 单元测试覆盖此 case

  **QA Scenarios**:
  ```
  Scenario: custom tonemap 解析成功
    Tool: Bash (ctest)
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --config Release -R cli
    Expected Result: 测试通过，包括 "tonemap=custom" 解析 case
    Evidence: .sisyphus/evidence/phase1/task-2-custom-tonemap.txt
  ```

  **Commit**: YES
  - Message: `fix(cli): parse --tonemap custom mode`
  - Files: `src/cli/cli_parser.cpp, tests/test_cli.cpp`
  - Pre-commit: `ctest --test-dir build -R cli`

- [ ] 3. NGX log 级别可配

  **What to do**:
  - **`src/cli/cli_parser.cpp`** + **`src/cli/config.h`**:
    - 新增 `--ngx-log-level <off|error|warn|info|verbose>` flag，默认 `warn`
    - Config 字段 `NVSDK_NGX_Logging_Level ngxLogLevel = NVSDK_NGX_LOGGING_LEVEL_OFF`（默认 OFF 更激进，或 WARN 保守）
  - **`src/dlss/ngx_wrapper.cpp:103`**:
    - 改硬编码 `NVSDK_NGX_LOGGING_LEVEL_VERBOSE` → 使用 `featureInfo.LoggingInfo.MinimumLoggingLevel = config.ngxLogLevel`
    - NgxContext::init 接受 log level 参数
  - **`src/main.cpp`** 或调用 `NgxContext::init` 处：传入 `config.ngxLogLevel`
  - 更新 `--help` 输出

  **Must NOT do**:
  - 不改我们自己的 logger（`src/core/logger.cpp`）
  - 不删 NGX log callback 函数

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES

  **References**:
  - `src/dlss/ngx_wrapper.cpp:101-104` log info 设置
  - `nvsdk_ngx_defs.h` NVSDK_NGX_Logging_Level enum

  **Acceptance Criteria**:
  - [ ] `dlss-compositor.exe --ngx-log-level off --test-ngx` 无 NGX 日志输出
  - [ ] `dlss-compositor.exe --ngx-log-level verbose --test-ngx` 有详细 NGX 输出
  - [ ] 默认（无 flag）行为 = WARN

  **QA Scenarios**:
  ```
  Scenario: log 级别生效
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --ngx-log-level off --test-ngx 2>&1 | findstr /i "\[NGX\]"
      2. 应 0 匹配
      3. build\Release\dlss-compositor.exe --ngx-log-level verbose --test-ngx 2>&1 | findstr /i "\[NGX\]" | Measure-Object
      4. 应 >0 匹配
    Expected Result: 行为符合预期
    Evidence: .sisyphus/evidence/phase1/task-3-log-levels.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add --ngx-log-level flag (default warn)`
  - Files: `src/cli/cli_parser.cpp, src/cli/config.h, src/dlss/ngx_wrapper.cpp, src/main.cpp`
  - Pre-commit: `ctest --test-dir build`

- [ ] 4. --source-fps + Frame Time Delta 派生

  **What to do**:
  - **`src/cli/cli_parser.cpp`** + **`src/cli/config.h`**:
    - 新增 `--source-fps <int>` flag，默认 30
    - Config 字段 `int sourceFps = 30;`
    - 注意命名：与现有 `--fps`（视频编码）严格区分
    - `--help` 文档中明确说明区别
  - **`src/dlss/dlss_sr_processor.h`**:
    - `DlssSRFrameInput` 新增 `float frameTimeDeltaMs = 33.3f;`
  - **`src/dlss/dlss_sr_processor.cpp:158`**:
    - 改 `evalParams.InFrameTimeDeltaInMsec = 33.3f;` → `evalParams.InFrameTimeDeltaInMsec = frame.frameTimeDeltaMs;`
  - **`src/pipeline/sequence_processor.cpp`** SR 路径填充帧输入处：
    - `frame.frameTimeDeltaMs = 1000.0f / static_cast<float>(config.sourceFps);`
  - 同样改动应用于 SR+FG 路径（如果 SR 用同样的 frame.frameTimeDeltaMs）
  - 检查 `--source-fps 0` 边界情况（除零保护：fallback 到 30）

  **Must NOT do**:
  - 不改 FG 内部时间逻辑（FG 不暴露此字段）
  - 不改 `--fps` flag 语义（仍是视频编码 fps）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES

  **References**:
  - `src/dlss/dlss_sr_processor.cpp:158` 当前硬编码点
  - `src/pipeline/sequence_processor.cpp` SR 帧准备处（约 line 794+）

  **Acceptance Criteria**:
  - [ ] `--source-fps 24` 后 verbose log 显示 `frameTimeDeltaMs ≈ 41.67`
  - [ ] `--source-fps 60` 后约 16.67
  - [ ] `--help` 区分 `--fps` 和 `--source-fps`
  - [ ] 默认 30fps，无 flag 时 = 33.3ms

  **QA Scenarios**:
  ```
  Scenario: source-fps 影响 frame time delta
    Tool: Bash + ctest
    Steps:
      1. cmake --build build --config Release
      2. build\Release\dlss-compositor.exe --source-fps 24 --input-dir tests/fixtures/sequence/ --output-dir tmp/ --scale 2 --ngx-log-level info 2>&1 | findstr /i "frametime\|41.67\|41.6"
      3. 应有匹配显示 41.67ms
    Expected Result: log 显示正确值
    Evidence: .sisyphus/evidence/phase1/task-4-source-fps.txt

  Scenario: 边界 source-fps 0 不崩
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --source-fps 0 --input-dir ... 
      2. 应 fallback 到默认 30 或报错友好
    Expected Result: 不崩溃
    Evidence: .sisyphus/evidence/phase1/task-4-zero-fps.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add --source-fps flag, derive InFrameTimeDeltaInMsec`
  - Files: `src/cli/*, src/dlss/dlss_sr_processor.*, src/pipeline/sequence_processor.cpp`
  - Pre-commit: `ctest --test-dir build`

- [ ] 5. Jitter 算法统一（回调写 camera.json）

  **What to do**:
  - **`blender/dlss_compositor_aov/__init__.py`**:
    - 在模块级新增 dict `_runtime_jitter = {}` 用于记录每帧实际施加的 jitter
    - 修改 `apply_jitter(scene)` 回调：
      ```python
      jitter_phases = ...  # 当前是 8（Task 6 改成动态）
      jitter_x = halton(scene.frame_current % jitter_phases, 2) - 0.5
      jitter_y = halton(scene.frame_current % jitter_phases, 3) - 0.5
      camera.data.shift_x = _original_shift_x + jitter_x / render_resolution_x
      camera.data.shift_y = _original_shift_y + jitter_y / render_resolution_y
      _runtime_jitter[scene.frame_current] = (jitter_x, jitter_y)
      ```
    - 修改 `export_camera` operator：
      - 优先从 `_runtime_jitter` 读取 jitter 值
      - 若 dict 无对应帧（用户未实际渲染），fallback 到当场计算（保持向后兼容）
      - 注释说明为什么用 dict（保证一致性）
  - **`blender/aov_export_preset.py`** 同步修改
  - 测试：用户场景 = 渲染 5 帧 → camera.json 中 jitter 值与 `apply_jitter` 应用值完全一致

  **Must NOT do**:
  - 不改 Halton 函数本身
  - 不改 jitter 范围 [-0.5, +0.5]
  - 不改 C++ 端读取逻辑

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocks**: Task 6（Task 6 也要改 apply_jitter，先做 5 让 6 在统一基础上改）

  **References**:
  - `blender/dlss_compositor_aov/__init__.py:75-80` 当前 jitter 应用
  - `blender/dlss_compositor_aov/__init__.py:168-180` 当前 export_camera

  **Acceptance Criteria**:
  - [ ] camera.json 中的 jitter 值与渲染回调实际施加的 byte-by-byte 一致
  - [ ] 未渲染情况下 export_camera 仍能工作（fallback）

  **QA Scenarios**:
  ```
  Scenario: 渲染后 jitter 一致性
    Tool: Blender MCP / headless
    Steps:
      1. blender --background --python tests/blender/test_jitter_consistency.py
      2. 渲染 8 帧并 export_camera
      3. 验证每帧 camera.json 的 jitter == apply_jitter dict 中记录值
    Expected Result: 完全一致
    Evidence: .sisyphus/evidence/phase1/task-5-jitter-consistency.txt
  ```

  **Commit**: YES
  - Message: `fix(blender): unify jitter calc — render handler is single source of truth`
  - Files: `blender/dlss_compositor_aov/__init__.py, blender/aov_export_preset.py`
  - Pre-commit: 手动 Blender 测试 + （如有）pytest

- [ ] 6. Jitter 相位按 scaleRatio² 计算

  **What to do**:
  - **Blender 端 UI**:
    - 在 `dlss_compositor_aov` 的 PropertyGroup 新增 `output_scale: bpy.props.FloatProperty(default=2.0, min=1.0, max=4.0)`
    - 在 panel 中暴露字段（Render Properties 下）
  - **`blender/dlss_compositor_aov/__init__.py`**:
    - 修改 `apply_jitter(scene)`:
      ```python
      scale = scene.dlss_compositor.output_scale
      jitter_phases = max(1, int(round(8 * scale * scale)))
      jitter_x = halton(scene.frame_current % jitter_phases, 2) - 0.5
      jitter_y = halton(scene.frame_current % jitter_phases, 3) - 0.5
      ```
    - 同步修改 export_camera（如未走 _runtime_jitter dict 的 fallback 路径）
    - 在 camera.json 顶层新增字段 `jitter_phases: int`，方便 C++ 端日志输出
  - **`blender/aov_export_preset.py`** 同步
  - **C++ 端（可选 verbose log）**:
    - `src/core/camera_data_loader.h` 新增 `int jitterPhases = 8;`
    - `src/core/camera_data_loader.cpp` 解析 `jitter_phases` 字段（向后兼容默认 8）
    - `src/pipeline/sequence_processor.cpp` SR 路径在 verbose log 输出实际相位数
  - **测试**:
    - 渲染 32 帧，验证 jitter 值在 32 个相位内不重复
    - scale=2.0 → phases=32, scale=1.5 → phases=18, scale=1.0 → phases=8

  **Must NOT do**:
  - 不改 Halton 函数
  - 不修改 C++ 的 jitter 应用代码（只读取，不重算）
  - 不破坏向后兼容（旧 camera.json 无 jitter_phases 字段也能工作）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1, after Task 5)
  **Blocked By**: Task 5（统一了 jitter 计算入口后再加变量）

  **References**:
  - vk_streamline `main.cpp:156-161` 公式来源
  - Task 5 的 `_runtime_jitter` dict
  - `src/core/camera_data_loader.h`

  **Acceptance Criteria**:
  - [ ] Blender 渲染 32 帧 scale=2.0 时，前 32 帧 jitter 值各不相同
  - [ ] scale=1.0 时仍是 8 相位（向后兼容）
  - [ ] camera.json 含 `jitter_phases` 字段
  - [ ] C++ 加载旧 camera.json（无字段）默认 8，不崩

  **QA Scenarios**:
  ```
  Scenario: 相位数随 scale 变化
    Tool: Bash (Python)
    Steps:
      1. 编写小脚本计算 expected_phases = round(8 * scale^2) for scale in [1.0, 1.5, 2.0, 3.0]
      2. 渲染对应序列
      3. 解析 camera.json 验证 jitter_phases == expected
    Expected Result: 8, 18, 32, 72
    Evidence: .sisyphus/evidence/phase1/task-6-phase-count.txt

  Scenario: 32 相位下 jitter 不重复
    Tool: Bash (Python)
    Steps:
      1. 渲染 32 帧 scale=2.0
      2. 解析 camera.json，提取 32 组 (jitter_x, jitter_y)
      3. 用 set() 检查重复
    Expected Result: 32 unique tuples
    Evidence: .sisyphus/evidence/phase1/task-6-no-repeat.txt

  Scenario: 向后兼容
    Tool: Bash (ctest)
    Steps:
      1. 用旧 camera.json fixture（无 jitter_phases）测 camera_data_loader
      2. 应解析成功，jitter_phases = 8
    Expected Result: 测试通过
    Evidence: .sisyphus/evidence/phase1/task-6-backward-compat.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): jitter phases = 8 × output_scale² per NVIDIA spec`
  - Files: `blender/dlss_compositor_aov/__init__.py, blender/aov_export_preset.py, src/core/camera_data_loader.*, src/pipeline/sequence_processor.cpp, tests/test_camera_data_loader.cpp`
  - Pre-commit: `ctest --test-dir build`

---

## Final Verification Wave

- [ ] F1. **计划合规审计** — `oracle`
  对每个 Must Have 验证。grep 确认 kNgxEngineVersion 单一来源、--source-fps 已加、tonemap custom 可解析、NGX log 级别可配、jitter 算法统一、相位数动态。

  **QA Scenarios**:
  ```
  Scenario: Must Have 全验证
    Tool: Bash + Grep
    Steps:
      1. Test-Path src/core/version.h  -- True
      2. Select-String -Path "src" -Pattern "kNgxEngineVersion" -Recurse | Measure-Object  -- 仅 1 (in version.h)
      3. build\Release\dlss-compositor.exe --tonemap custom --tonemap-lut x --inverse-tonemap-lut y 不报 "unknown"
      4. build\Release\dlss-compositor.exe --ngx-log-level off --test-ngx 无 NGX 日志
      5. build\Release\dlss-compositor.exe --source-fps 24 ... 日志显 41.67
    Expected Result: 全部满足
    Evidence: .sisyphus/evidence/phase1/F1-must-have.txt
  ```

- [ ] F2. **代码质量审查** — `unspecified-high`
  Build + 全套测试。检查无 dead code、无重复常量、新增 flag 在 --help 中正确显示。

- [ ] F3. **真实 QA** — `unspecified-high`
  从 Blender 渲染一段 16 帧序列（scale=2，所以 32 相位）。前 16 帧 jitter 值无重复。跑 compositor 处理，verbose log 显示正确 frame_time 和 jitter_phases。

- [ ] F4. **范围保真度检查** — `deep`
  验证未误改 SR/FG 核心逻辑。git diff 范围与 Concrete Deliverables 一致。

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `refactor(version): consolidate engine version into core/version.h` | src/core/version.h, src/dlss/ngx_wrapper.cpp, src/gpu/vulkan_context.cpp | ctest |
| 2 | `fix(cli): parse --tonemap custom mode` | src/cli/cli_parser.cpp, tests/test_cli.cpp | ctest -R cli |
| 3 | `feat(cli): add --ngx-log-level flag (default warn)` | src/cli/*, src/dlss/ngx_wrapper.cpp, src/main.cpp | ctest |
| 4 | `feat(cli): add --source-fps flag, derive InFrameTimeDeltaInMsec` | src/cli/*, src/dlss/dlss_sr_processor.*, src/pipeline/sequence_processor.cpp | ctest |
| 5 | `fix(blender): unify jitter calc — render handler as single source` | blender/* | manual blender test |
| 6 | `feat(blender): jitter phases = 8 × output_scale² per NVIDIA spec` | blender/*, src/core/camera_data_loader.*, src/pipeline/sequence_processor.cpp, tests/test_camera_data_loader.cpp | ctest |

---

## Success Criteria

### Verification Commands
```bash
ctest --test-dir build --config Release --output-on-failure
pytest tests/ -v

# Version 一致
dlss-compositor.exe --version  # 显示 0.2.1

# CLI 新 flag 工作
dlss-compositor.exe --tonemap custom --help  # 不报 unknown
dlss-compositor.exe --source-fps 24 --ngx-log-level info ... 2>&1 | findstr /i "41"

# Blender 验证（需 RTX + Blender）
blender --background --python tests/blender/test_jitter_consistency.py
```

### Final Checklist
- [ ] 6 个独立 commit 全部成功
- [ ] 测试无回归
- [ ] camera.json 字段向后兼容
- [ ] CLI --help 反映新 flag
