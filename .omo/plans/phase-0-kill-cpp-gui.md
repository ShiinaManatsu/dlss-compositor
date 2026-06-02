# Phase 0 — Kill C++ ImGui GUI

## TL;DR

> **目标**: 删除 `src/ui/` 中所有 C++ ImGui+Vulkan GUI 代码（~1370 LOC），收缩 CLI 表面（去 4 个 test/gui flag），简化 Vulkan setup（去 swapchain/surface），让 C++ 二进制聚焦纯 batch CLI 处理器。
>
> **背景**: Electron GUI（`gui/`）已成为唯一用户界面。C++ ImGui GUI 是开发期脚手架，已无生产价值。
>
> **交付物**:
> - 删除 `src/ui/` 整个目录
> - CLI 移除 `--gui` / `--test-gui` / `--test-load` / `--test-process`
> - CMakeLists 移除 ImGui + GLFW 依赖
> - Vulkan context 简化（仅 compute queue，无 surface/swapchain）
> - 文档（README + docs）更新
> - 验证 Electron GUI 不再调用已删 flag
>
> **预估工作量**: M（中等，跨多模块但都是删除）
> **并行执行**: YES — 2 波次
> **关键路径**: Task 1（Electron 兼容性审计）→ Task 2-7（并行删除）→ Task 8（验证 + 文档）

---

## Context

### Original Request
Phase 0 of the post-stable cleanup. User said: "把c++的gui干掉, dlss这块只保留cli"。Electron GUI 在 `gui/` 已完全独立成熟，C++ 端的 ImGui GUI 是早期开发产物，现已是死重。

### Background
- 项目已演进到 0.2.1，FG + SR + Electron GUI 都稳定
- C++ 端 GUI 代码 ~1370 LOC：app.cpp/h, image_viewer.cpp/h, settings_panel.cpp/h
- Electron GUI 通过 spawn `dlss-compositor.exe` 子进程方式调用 CLI
- Phase 0 必须最先做：之后所有改动（删 GBuffer / Sharpen / 等）都不再受 ImGui 兼容性拖累

### 已确认的范围
- ✅ 删除 `src/ui/` 全部
- ✅ 删除相关 CLI flag
- ✅ 删除 ImGui / GLFW CMake 依赖
- ✅ 简化 Vulkan context（去 surface/swapchain/present queue）
- ❌ **不**删除 Electron GUI（`gui/`，那是用户界面）
- ❌ **不**改动 DLSS-SR / DLSS-FG 处理代码

---

## Work Objectives

### Core Objective
让 `dlss-compositor.exe` 成为纯 CLI batch 处理器，无任何窗口/UI 代码。Electron GUI 通过 spawn 调用其 CLI 接口。

### Concrete Deliverables
- 删除文件清单：
  - `src/ui/app.cpp`, `src/ui/app.h`
  - `src/ui/image_viewer.cpp`, `src/ui/image_viewer.h`
  - `src/ui/settings_panel.cpp`, `src/ui/settings_panel.h`
  - `src/ui/.gitkeep`（如目录已空）
- 修改文件清单：
  - `src/cli/cli_parser.cpp` 移除 4 个 flag 解析
  - `src/cli/config.h` 移除对应字段
  - `src/main.cpp` 移除 GUI 分支
  - `CMakeLists.txt` 移除 ImGui / GLFW
  - `src/gpu/vulkan_context.cpp` 简化（去 swapchain）
- 更新 README, docs 反映 GUI 现状

### Definition of Done
- [ ] `cmake --build build --config Release` 成功，二进制不含 ImGui/GLFW 符号
- [ ] `ctest --test-dir build --output-on-failure` 全部通过
- [ ] `dlss-compositor.exe --help` 不再列出 `--gui` / `--test-*` 相关 flag
- [ ] Electron GUI 启动 + 跑一次完整 SR+FG 处理无错
- [ ] 二进制体积比改前小（验证 ImGui/GLFW 已不链接）

### Must Have
- 完全删除 `src/ui/` 目录
- CLI 不再暴露 GUI 相关 flag
- Electron GUI 仍能正常工作
- 所有现有 SR/FG/EXR 测试仍通过

### Must NOT Have (Guardrails)
- ❌ 不修改 SR / FG / channel mapper / mv converter / camera loader 等核心逻辑
- ❌ 不改动 Electron GUI 代码（`gui/` 目录不动，只验证它仍工作）
- ❌ 不在本 phase 做 CLI 的其他改动（custom tonemap / source-fps 等留给 Phase 1）
- ❌ 不删除任何 shader 文件（pq_transfer.comp / apply_lut.comp 与 GUI 无关）
- ❌ 不删除 NGX wrapper 中的 verbose log（log 级别调整留给 Phase 1）
- ❌ 不删除 texture pool / async writer 等纯计算 GPU 设施

---

## Verification Strategy

### Test Decision
- **基础设施**: YES（Catch2 for C++, pytest for Python, Playwright for Electron e2e）
- **自动化测试**: YES (Tests-after — 删除后跑全套回归)
- **框架**: 现有

### QA Policy
- **删除验证**: 用 `dir` / `Get-ChildItem` 确认文件不存在
- **构建验证**: `cmake --build` 和 `ctest`
- **运行验证**: `dlss-compositor.exe --help` 无 GUI flag；`--test-ngx` 仍工作
- **集成验证**: 跑一个真实 EXR 序列（SR-only 和 SR+FG）
- **Electron 验证**: `gui/` 启动 + 处理一个序列

证据保存到 `.sisyphus/evidence/phase0/task-{N}-{slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 0 (前置审计 — 必须先完成):
└── Task 1: Electron 兼容性审计（不动代码）

Wave 1 (并行删除 — Wave 0 通过后):
├── Task 2: 删除 src/ui/ 文件
├── Task 3: 收缩 CLI parser 和 config
├── Task 4: 简化 main.cpp
├── Task 5: CMakeLists 移除 ImGui/GLFW
└── Task 6: 简化 Vulkan context（去 swapchain）

Wave 2 (验证 — Wave 1 完成后):
├── Task 7: 测试与回归（ctest + pytest）
└── Task 8: 文档更新 + Electron e2e 验证

Wave FINAL (审查):
├── F1: 计划合规审计 (oracle)
├── F2: 代码质量审查 (unspecified-high)
├── F3: 真实 QA (unspecified-high)
└── F4: 范围保真度检查 (deep)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 2-6 | 0 |
| 2 | 1 | 7 | 1 |
| 3 | 1 | 7 | 1 |
| 4 | 1 | 7 | 1 |
| 5 | 1 | 7 | 1 |
| 6 | 1 | 7 | 1 |
| 7 | 2-6 | 8 | 2 |
| 8 | 7 | F1-F4 | 2 |

### Agent Dispatch Summary

- **Wave 0**: 1 task — T1 → `quick`
- **Wave 1**: 5 tasks — T2-T6 → `quick` (并行)
- **Wave 2**: 2 tasks — T7 → `unspecified-high`, T8 → `writing`
- **FINAL**: 4 tasks — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. Electron 兼容性审计（前置 gate）

  **What to do**:
  - 在 `gui/electron/` 和 `gui/src/` 中 grep 是否有调用 `--gui`, `--test-gui`, `--test-load`, `--test-process` 的代码
  - 如果有，记录调用点和上下文，但**不修改**——本 phase 仅审计
  - 如果发现 Electron 真的调用了某个 flag，记录为风险并在 Task 8 决定如何处理（最可能是 Electron 未来要更新调用方式）
  - 输出审计报告到 `.sisyphus/evidence/phase0/task-1-electron-audit.txt`
  - 同时检查 `gui/package.json` 的 spawn 逻辑、`electron/main.ts` 等

  **Must NOT do**:
  - 不修改 Electron 代码
  - 不修改 C++ 代码

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO（前置 gate）
  - **Blocks**: Tasks 2-6
  - **Blocked By**: None

  **References**:
  - `gui/electron/` — Electron 主进程代码
  - `gui/src/` — Renderer 进程代码
  - `gui/package.json` — 启动配置

  **Acceptance Criteria**:
  - [ ] 输出审计报告，列出 Electron 中所有 spawn `dlss-compositor.exe` 的位置和参数
  - [ ] 明确结论：Electron 是否依赖被删的 4 个 flag

  **QA Scenarios**:
  ```
  Scenario: 审计报告生成
    Tool: Bash (Select-String / Get-Content)
    Steps:
      1. Select-String -Path "gui\electron\**\*.ts","gui\src\**\*.ts","gui\src\**\*.tsx" -Pattern "test-gui|test-load|test-process|--gui\b" -SimpleMatch:$false
      2. 把结果写入 .sisyphus/evidence/phase0/task-1-electron-audit.txt
      3. 同时记录所有 spawn dlss-compositor.exe 的 args 数组
    Expected Result: 报告生成；明确结论"无依赖"或"依赖以下 flag: ..."
    Failure Indicators: 发现 Electron 依赖 --gui 或 --test-gui（需在 Task 8 协调）
    Evidence: .sisyphus/evidence/phase0/task-1-electron-audit.txt
  ```

  **Commit**: NO（仅审计）

- [x] 2. 删除 src/ui/ 文件

  **What to do**:
  - 删除以下文件：
    - `src/ui/app.cpp` (~598 行)
    - `src/ui/app.h` (~87 行)
    - `src/ui/image_viewer.cpp` (~359 行)
    - `src/ui/image_viewer.h` (~71 行)
    - `src/ui/settings_panel.cpp` (~194 行)
    - `src/ui/settings_panel.h` (~62 行)
    - `src/ui/.gitkeep`（保留目录占位用，可删可不删——倾向删除）
  - 用 git rm 而不是 OS 删除，保留 git 历史
  - 如果删除后 `src/ui/` 为空，删除目录
  - 验证：`Get-ChildItem src/ui` 应该报错或空

  **Must NOT do**:
  - 不修改其他模块代码
  - 不修改 CMakeLists（Task 5 做）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (Wave 1)
  - **Blocks**: Task 7
  - **Blocked By**: Task 1

  **References**:
  - 文件清单见 What to do

  **Acceptance Criteria**:
  - [ ] `src/ui/` 目录不存在或为空
  - [ ] `git status` 显示 6 个删除的文件

  **QA Scenarios**:
  ```
  Scenario: 文件已删除
    Tool: Bash
    Steps:
      1. git rm src/ui/app.cpp src/ui/app.h src/ui/image_viewer.* src/ui/settings_panel.* src/ui/.gitkeep
      2. Test-Path src/ui  -- 如果 True 且为空, Remove-Item src/ui
      3. git status
    Expected Result: 6+ deletions in git status
    Evidence: .sisyphus/evidence/phase0/task-2-deletions.txt
  ```

  **Commit**: NO（最终在 Task 7 通过测试后一起 commit）

- [x] 3. CLI parser 和 config 收缩

  **What to do**:
  - **`src/cli/cli_parser.cpp`**:
    - 找到 `--gui` 解析分支并删除
    - 找到 `--test-gui` 解析分支并删除
    - 找到 `--test-load` 解析分支并删除
    - 找到 `--test-process` 解析分支并删除
    - 同时删除 `printHelp()` 中对应的 flag 描述
    - 同时删除 `printVersion()` 如果引用了 GUI
  - **`src/cli/config.h`**:
    - 删除对应字段（可能包括 `bool launchGui`, `bool testGui`, `std::string testLoadPath`, `bool testProcess` 等）
    - 检查是否有相关结构体/枚举需清理
  - **`tests/test_cli.cpp`**:
    - 删除测试这 4 个 flag 的用例

  **Must NOT do**:
  - 不删除其他 flag
  - 不修改其他模块

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (Wave 1)
  - **Blocks**: Task 7
  - **Blocked By**: Task 1

  **References**:
  - `src/cli/cli_parser.cpp` — 主解析逻辑
  - `src/cli/config.h` — Config struct
  - `tests/test_cli.cpp` — CLI 测试

  **Acceptance Criteria**:
  - [ ] `grep "test-gui\|test-load\|test-process\|--gui\b" src/cli/` 无结果
  - [ ] `tests/test_cli.cpp` 中无对应测试

  **QA Scenarios**:
  ```
  Scenario: CLI flags 已移除
    Tool: Grep
    Steps:
      1. Select-String -Path "src\cli\*.cpp","src\cli\*.h" -Pattern "test-gui|test-load|test-process"
      2. 应无匹配（除注释或字符串文档外）
    Expected Result: 0 matches in production code
    Evidence: .sisyphus/evidence/phase0/task-3-flags-removed.txt
  ```

  **Commit**: NO（与其他 Wave 1 任务一起 commit）

- [x] 4. main.cpp 简化

  **What to do**:
  - **`src/main.cpp`**:
    - 删除 `#include "ui/app.h"` 和其他 ui/ 头
    - 删除 `if (config.launchGui)` 分支
    - 删除 `if (config.testGui)` 分支
    - 删除 `if (config.testLoadPath ...)` 分支
    - 删除 `if (config.testProcess)` 分支
    - 保留 `--test-ngx`, `--test-vulkan` 这些非 GUI 测试模式
    - 整体逻辑应该简化为：parse → 根据模式（test-ngx/test-vulkan/batch-process）分发

  **Must NOT do**:
  - 不修改 batch processing 主流程
  - 不修改 NGX/Vulkan 测试逻辑

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (Wave 1)
  - **Blocks**: Task 7
  - **Blocked By**: Task 1

  **References**:
  - `src/main.cpp` 主入口

  **Acceptance Criteria**:
  - [ ] `grep "ui/" src/main.cpp` 无结果
  - [ ] main.cpp 编译通过

  **QA Scenarios**:
  ```
  Scenario: main.cpp 无 ui/ 引用
    Tool: Grep
    Steps:
      1. Select-String -Path "src\main.cpp" -Pattern "ui/|launchGui|testGui|testLoad|testProcess"
    Expected Result: 0 matches
    Evidence: .sisyphus/evidence/phase0/task-4-main-clean.txt
  ```

  **Commit**: NO

- [x] 5. CMakeLists 移除 ImGui / GLFW

  **What to do**:
  - **`CMakeLists.txt`**:
    - 删除 `FetchContent_Declare` 或 `find_package` 中的 imgui
    - 删除 GLFW 相关 declare/find
    - 删除 `add_executable` 或 `target_sources` 中对 `src/ui/*.cpp` 的引用
    - 删除 `target_link_libraries` 中的 imgui / glfw
    - 同时检查 `src/CMakeLists.txt`（如果有 subdirectory）
  - 验证：再次 `cmake -B build` 时不再下载/链接 ImGui/GLFW

  **Must NOT do**:
  - 不删除 Vulkan SDK / VMA / volk / tinyexr / Catch2 等
  - 不修改测试 CMake 目标

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (Wave 1)
  - **Blocks**: Task 7
  - **Blocked By**: Task 1

  **References**:
  - `CMakeLists.txt`
  - 任何 `src/CMakeLists.txt` 子文件

  **Acceptance Criteria**:
  - [ ] `grep -i "imgui\|glfw" CMakeLists.txt` 无结果
  - [ ] 干净 build dir 重新 configure 不下载 imgui/glfw

  **QA Scenarios**:
  ```
  Scenario: CMake 不再引用 ImGui/GLFW
    Tool: Grep
    Steps:
      1. Select-String -Path "CMakeLists.txt","src\**\CMakeLists.txt" -Pattern "imgui|glfw|ImGui|GLFW" -SimpleMatch:$false
    Expected Result: 0 matches
    Evidence: .sisyphus/evidence/phase0/task-5-cmake-clean.txt

  Scenario: Clean rebuild 不下载 ImGui/GLFW
    Tool: Bash
    Steps:
      1. Remove-Item -Recurse -Force build_clean -ErrorAction SilentlyContinue
      2. cmake -B build_clean -G "Visual Studio 17 2022" 2>&1 | findstr /i "imgui glfw"
    Expected Result: 无 imgui/glfw 下载或配置输出
    Evidence: .sisyphus/evidence/phase0/task-5-no-deps.txt
  ```

  **Commit**: NO

- [x] 6. Vulkan context 简化（去 swapchain / surface）

  **What to do**:
  - **`src/gpu/vulkan_context.cpp`**:
    - 检查并删除 `VK_KHR_swapchain` 扩展请求（仅 GUI 需要）
    - 检查并删除 `VK_KHR_surface` 扩展请求
    - 检查并删除 GLFW surface 创建相关代码（如有）
    - 简化设备选择：不再要求"必须支持 graphics + present queue"，只要 compute queue
    - 保留 graphics queue 的其他用途（pq_transfer 等仍需要）
  - **`src/gpu/vulkan_context.h`**:
    - 删除任何 surface/swapchain 相关的成员变量或方法
  - **注意**: 如果某些 Vulkan 扩展（如 `VK_KHR_get_physical_device_properties2`）被 ImGui 间接需要，但其他模块也用，保留之

  **Must NOT do**:
  - 不删除 `VK_NV_optical_flow` 等 NGX 必需扩展
  - 不删除 VMA / volk 初始化
  - 不修改 texture pool / texture pipeline

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (Wave 1)
  - **Blocks**: Task 7
  - **Blocked By**: Task 1

  **References**:
  - `src/gpu/vulkan_context.cpp` / `.h`
  - 参考 nvpro-samples vk_streamline 的 headless 启动模式

  **Acceptance Criteria**:
  - [ ] Vulkan context 初始化成功（test-vulkan smoke test 通过）
  - [ ] 二进制不再链接 swapchain 相关符号
  - [ ] `--test-ngx` 仍正常通过

  **QA Scenarios**:
  ```
  Scenario: Vulkan context 仍能初始化
    Tool: Bash
    Steps:
      1. cmake --build build --config Release
      2. build\Release\dlss-compositor.exe --test-vulkan
    Expected Result: Exit 0, "Vulkan initialized" in output
    Failure Indicators: 退出码非 0 或缺少必要扩展报错
    Evidence: .sisyphus/evidence/phase0/task-6-vulkan-init.txt

  Scenario: NGX 仍可用
    Tool: Bash (RTX GPU 上)
    Steps:
      1. build\Release\dlss-compositor.exe --test-ngx
    Expected Result: Exit 0, "DLSS-SR available: true"
    Evidence: .sisyphus/evidence/phase0/task-6-ngx-still-works.txt
  ```

  **Commit**: NO

- [x] 7. 全套测试与回归

  **What to do**:
  - **C++ 测试**:
    - `cmake --build build --config Release` 必须成功
    - `ctest --test-dir build --config Release --output-on-failure` 全过
    - 删除任何引用 src/ui 的测试（test_app.cpp / test_image_viewer.cpp 等如果存在）
  - **Python 测试**:
    - `pytest tests/ -v` 全过
  - **CLI smoke**:
    - `dlss-compositor.exe --help` 验证 flag 列表
    - `dlss-compositor.exe --test-vulkan` 通过
    - `dlss-compositor.exe --test-ngx` 通过
  - **真实 EXR 处理**（如有 fixture 序列）:
    - 跑一个 SR-only 处理验证
    - 跑一个 SR+FG 处理验证
  - **二进制体积对比**:
    - 记录改前/改后的 `dlss-compositor.exe` 大小
    - 期望减小 10-20%

  **Must NOT do**:
  - 不修改测试代码（除删除 GUI 相关测试外）
  - 不跳过任何失败测试

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO（必须 Wave 1 全完成）
  - **Blocks**: Task 8
  - **Blocked By**: Tasks 2-6

  **References**:
  - 所有 tests/ 文件
  - CMake build target 列表

  **Acceptance Criteria**:
  - [ ] Build 0 errors, 0 new warnings
  - [ ] All ctest pass (excluding GUI-specific tests if present)
  - [ ] All pytest pass
  - [ ] `--help` 不列出 GUI flag
  - [ ] `--test-ngx` / `--test-vulkan` 通过
  - [ ] 二进制 size 减小

  **QA Scenarios**:
  ```
  Scenario: 完整构建 + 测试通过
    Tool: Bash
    Steps:
      1. Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
      2. cmake -B build -G "Visual Studio 17 2022"
      3. cmake --build build --config Release
      4. ctest --test-dir build --config Release --output-on-failure
      5. pytest tests/ -v
    Expected Result: 所有命令 exit 0
    Evidence: .sisyphus/evidence/phase0/task-7-full-test.txt

  Scenario: CLI smoke
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --help | findstr /i "gui test-gui test-load test-process"
      2. 期望 0 匹配
      3. build\Release\dlss-compositor.exe --test-vulkan
      4. build\Release\dlss-compositor.exe --test-ngx (RTX 必需)
    Expected Result: GUI flag 全部消失；test-vulkan/ngx 仍工作
    Evidence: .sisyphus/evidence/phase0/task-7-cli-smoke.txt

  Scenario: 二进制体积验证
    Tool: Bash
    Steps:
      1. (Get-Item build\Release\dlss-compositor.exe).Length
      2. 与 Phase 0 之前的 baseline 对比（commit cfa4ec7 时的大小）
    Expected Result: 减小 5-25%
    Evidence: .sisyphus/evidence/phase0/task-7-binary-size.txt
  ```

  **Commit**: YES（Wave 1 + Wave 2 一起）
  - Message: `refactor: remove C++ ImGui GUI, simplify Vulkan to compute-only`
  - Files: `src/ui/* (deleted), src/cli/*, src/main.cpp, src/gpu/vulkan_context.*, CMakeLists.txt, tests/test_cli.cpp`
  - Pre-commit: `cmake --build build --config Release && ctest --test-dir build --output-on-failure`

- [x] 8. 文档更新 + Electron e2e 验证

  **What to do**:
  - **README.md** 更新：
    - 删除 "Launch GUI: `dlss-compositor.exe --gui`" 之类表述
    - 改为："GUI 通过单独的 Electron 应用启动 (`dlss-compositor-gui.exe`)"
    - 更新 CLI 用法示例不再包含 GUI flag
  - **`docs/usage_guide.md`** / **`docs/build_guide.md`** 同步更新
  - **`scripts/package-release.ps1`** 检查：仍能正确打包（不应包含 ImGui dll，本来也没有）
  - **Electron e2e 验证**:
    - `cd gui && npm install`（如未装）
    - 启动 Electron app: `npm run dev` 或类似
    - 在 Electron GUI 内配置一个真实序列处理（如 fixture），点击 Start
    - 观察 Electron GUI 是否成功 spawn 新二进制并完成处理
    - 截图作为证据
  - 如果 Task 1 审计发现 Electron 依赖被删的 flag → 在本任务中协调（向用户提示需后续 Phase 处理 Electron 端，或在本 phase 一并改）

  **Must NOT do**:
  - 不重写 Electron 应用
  - 不修改 Electron 端的 spawn 参数（除非 Task 1 发现冲突）

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Blocks**: F1-F4
  - **Blocked By**: Task 7

  **References**:
  - `README.md`
  - `docs/usage_guide.md`, `docs/build_guide.md`
  - `gui/` Electron 应用
  - Task 1 审计报告

  **Acceptance Criteria**:
  - [ ] README 不再提及 C++ GUI flag
  - [ ] docs/ 各文件同步
  - [ ] Electron app 能成功跑通一个真实处理任务

  **QA Scenarios**:
  ```
  Scenario: 文档无 stale 引用
    Tool: Grep
    Steps:
      1. Select-String -Path "README.md","docs\*.md" -Pattern "--gui\b|--test-gui|--test-load|--test-process"
    Expected Result: 0 matches (in production docs)
    Evidence: .sisyphus/evidence/phase0/task-8-docs-clean.txt

  Scenario: Electron app e2e
    Tool: Bash + Manual / Playwright (如已配)
    Steps:
      1. cd gui; npm install (if needed)
      2. npm run dev (or start packaged)
      3. 在 GUI 中配置 fixture 序列 + 点击 Start
      4. 观察处理日志、进度、最终输出
    Expected Result: 成功完成处理；截图证据
    Evidence: .sisyphus/evidence/phase0/task-8-electron-run.png + log
  ```

  **Commit**: YES
  - Message: `docs: update for C++ GUI removal; Electron is sole user interface`
  - Files: `README.md, docs/*.md`
  - Pre-commit: 文档 lint（如配置）

---

## Final Verification Wave

> 4 个审查 agent 并行运行。全部通过后向用户展示并获明确 okay。

- [x] F1. **计划合规审计** — `oracle`
  对每个 "Must Have"：验证已删除（grep 验证）。对每个 "Must NOT Have"：搜索代码确认未误删 SR/FG/EXR 等核心。检查 evidence 文件齐全。
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: Must Have 验证
    Tool: Bash
    Steps:
      1. Test-Path src/ui  -- 应 False
      2. Select-String -Path "src\cli\*.cpp" -Pattern "test-gui|test-load|test-process|--gui\b"  -- 应 0 匹配
      3. Select-String -Path "CMakeLists.txt" -Pattern "imgui|glfw" -SimpleMatch:$false  -- 应 0 匹配
      4. build\Release\dlss-compositor.exe --test-ngx  -- 应 exit 0
    Expected Result: 全部满足
    Evidence: .sisyphus/evidence/phase0/F1-must-have.txt

  Scenario: Must NOT Have 验证
    Tool: Bash
    Steps:
      1. Test-Path src/dlss/dlss_sr_processor.cpp  -- 应 True
      2. Test-Path src/dlss/dlss_fg_processor.cpp  -- 应 True
      3. Test-Path src/core/channel_mapper.cpp  -- 应 True
      4. Test-Path gui  -- 应 True
    Expected Result: 核心模块未被误删
    Evidence: .sisyphus/evidence/phase0/F1-must-not.txt
  ```

- [x] F2. **代码质量审查** — `unspecified-high`
  Build + ctest + pytest。检查无遗留 dead code（include "ui/app.h" 等）。
  Output: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: 全套质量检查
    Tool: Bash
    Steps:
      1. cmake --build build --config Release 2>&1 | findstr /i "error warning"
      2. ctest --test-dir build --config Release --output-on-failure
      3. pytest tests/ -v
      4. Select-String -Path "src\**\*.cpp","src\**\*.h" -Pattern "ui/app|ImGui|GLFW" -SimpleMatch:$false
    Expected Result: 0 errors, 0 stale references
    Evidence: .sisyphus/evidence/phase0/F2-quality.txt
  ```

- [x] F3. **真实 QA** — `unspecified-high`
  执行所有 task 的 QA 场景。集成测试：完整 SR-only + SR+FG 处理 fixture 序列。
  Output: `Scenarios [N/N pass] | Integration [N/N] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: 完整 e2e
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir tmp_e2e/ --scale 2
      2. dir tmp_e2e/*.exr | Measure-Object  -- 应有输出
      3. （如 RTX 可用）跑一段 SR+FG 序列
    Expected Result: 输出文件正确生成
    Evidence: .sisyphus/evidence/phase0/F3-e2e.txt
  ```

- [x] F4. **范围保真度检查** — `deep`
  对每个 task 读 spec 和 diff，验证 1:1。检查 Must NOT 合规。
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N] | VERDICT`

  **QA Scenarios**:
  ```
  Scenario: 范围审计
    Tool: Bash + Git
    Steps:
      1. git diff --stat HEAD~1 HEAD  -- 列出所有改动文件
      2. 对比与 Plan 中 Concrete Deliverables 列表
      3. 任何超出列表的文件都需说明
    Expected Result: 改动文件全部在预期范围内
    Evidence: .sisyphus/evidence/phase0/F4-scope.txt
  ```

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `refactor: remove C++ ImGui GUI, simplify Vulkan to compute-only` | src/ui/*(deleted), src/cli/*, src/main.cpp, src/gpu/vulkan_context.*, CMakeLists.txt, tests/test_cli.cpp | ctest --test-dir build |
| 2 | `docs: update for C++ GUI removal; Electron is sole user interface` | README.md, docs/*.md | — |

> **注**：Wave 1 的 5 个 task（2-6）应作为单一 commit 提交，因为它们共同构成"删除 C++ GUI"的原子操作。Task 7 的测试是 commit 前的 gate。

---

## Success Criteria

### Verification Commands
```bash
# Build 干净
cmake --build build --config Release  # Expected: BUILD SUCCEEDED, no warnings

# 所有测试
ctest --test-dir build --config Release --output-on-failure  # Expected: 100% pass
pytest tests/ -v  # Expected: 100% pass

# CLI 表面已收缩
dlss-compositor.exe --help | findstr /i "gui test-gui test-load test-process"  # Expected: 0 matches

# NGX 仍工作
dlss-compositor.exe --test-ngx  # Expected: exit 0, "DLSS-SR available: true"

# Electron GUI 仍能驱动 CLI
cd gui; npm run dev  # 手动配置 + 点击 Start，观察处理成功
```

### Final Checklist
- [ ] 所有 "Must Have" 满足
- [ ] 所有 "Must NOT Have" 未误触
- [ ] C++ + Python 测试全过
- [ ] Electron GUI e2e 通过
- [ ] 文档更新完毕
- [ ] 二进制体积减小
- [ ] 用户视觉确认 README 阅读体验良好
