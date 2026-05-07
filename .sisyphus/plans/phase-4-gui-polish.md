# Phase 4 — Electron GUI Polish & Bidirectional CLI Integration

## TL;DR

> **目标**: 整体翻新 Electron GUI，让 SR 配置更直观（4K vs 1.5x 任选）、camera.json 自动发现、live frame preview 真正显示画面、CLI 输出捕获更稳。同时把 Phases 1-3 新增的 CLI flag（`--source-fps` / `--sharpness` / `--ngx-log-level` / `--tonemap custom`）暴露到 GUI。**双向**改动：GUI 改进发现的 CLI 痛点反向传播（camera.json auto-discovery、NDJSON progress、可选 dry-run）。
>
> **交付物**:
> - GUI: 暴露 4 个新 CLI flag、target resolution + scale 双向联动、camera.json 自动填充、live frame preview（FFmpeg 转 PNG）、NGX banner 头部展示、ETA 计算、改进表单流
> - CLI: 自动发现 `<input-dir>/camera.json`、`--progress-format json` NDJSON 输出、可选 `--dry-run`
> - Tests: Playwright 覆盖新控件 + IPC 测试 + CLI NDJSON 解析单测
> - Docs: README/usage_guide 更新 GUI 流程
>
> **预估工作量**: L (跨 GUI + CLI 双 stack)
> **并行执行**: YES — 5 波次
> **关键路径**: CLI 反向改动（camera 自动 + NDJSON）→ GUI 集成新 IPC + 控件 → live preview → UX polish → 测试

**前置**: Phases 0-3 全部完成（CLI 已有 `--source-fps` / `--sharpness` / `--ngx-log-level` / `--tonemap custom` 等 flag）

---

## Context

### Original Request
今天讨论后用户提了 5 个 GUI 改进点 + 明确指出"会反向传播给 CLI 的改动"：

1. **fps 配置 + 输出分辨率/缩放联动**：用户想用 "4K" 或 "1.5x" 任一形式表达期望输出
2. **camera.json 自动填充**：选完 EXR 目录后如果同目录有 camera.json，自动填到对应输入框
3. **Live frame preview**：右侧进度栏当前不显示最新 EXR 帧（仅文件名 + 计数器）
4. **整体 UX 易用性**：减少按钮数、智能默认、清晰错误提示
5. **CLI 参数 + 输出捕获同步**：Phases 1-3 新增的 flag 需要在 GUI 暴露；CLI 输出格式需要稳定可靠（当前 regex 脆弱）

### Background — GUI 现状审计要点
（来自 explore agent 报告 ses_203820364）

**已完成的部分** (✅)：
- React 18 + TypeScript + Tailwind + Vite + Electron 28 stack 稳定
- 25 个 .ts/.tsx 文件，~1700 LOC 总量
- 12 个 IPC channel
- 11 个 form validation 规则
- 4 个 Playwright e2e 测试 spec (372 LOC)
- electron-store 持久化 + React Context 状态
- Frame preview "组件"已存在 (`ProcessingView.tsx:229-256`) 但实际**只显示文件名和帧计数器**，无图像

**已识别的差距** (❌)：
- `--source-fps` / `--sharpness` 在 GUI 中**完全缺失**
- `tonemapMode` select 仅 'pq' | 'none'，**缺 'custom'**
- 进度解析正则 `/Processing frame(?: pair)? (\d+)\/(\d+)/` 在 `electron/progress-parser.ts:5-10`——**脆弱**
- NGX banner（`[NGX] DLSS-SR: available` 等，Phase 2 Task 4 新增）**完全未捕获**
- Frame preview 是 3 秒轮询 `frame:latest`（`main.ts:150-161` 扫目录返回最新 EXR 路径）——**无图像渲染**
- Camera.json 必须用户手动选择，无自动发现
- 无 ETA 计算
- 无 dry-run / pre-flight validation 模式

### CLI 反向传播改动（已与用户确认方向）
今日讨论中明确将**部分 CLI 改动**纳入 Phase 4 范围：
- ✅ CLI auto-discover camera.json from `<input-dir>/camera.json` 当 `--camera-data` 未指定
- ✅ `--progress-format json` 机器可读 NDJSON 输出（GUI 解析更稳）
- ✅ NDJSON 含 `frame_written` 事件（含路径，驱动 live preview，无需轮询）
- 🟡 `--dry-run` 验证模式（可选，方便 GUI 加 "Validate" 按钮）
- ❌ **不**加 `--target-resolution`（GUI 端从 camera.json 读 render_w/h 算 scale，传 `--scale`，CLI 不变）

### 已确认的设计决策
- ✅ Frame preview 用 **FFmpeg CLI 转 PNG**（FFmpeg 已是 video encoding 依赖，无新依赖）
- ✅ Resolution + scale 联动：GUI 从 camera.json 读 `render_width/render_height` 自动检测源分辨率；用户手动输入或选预设 (4K/1440p/1080p)；GUI 计算 scale 发给 CLI
- ✅ NDJSON 是默认人读格式之外的 opt-in（`--progress-format json`），向后兼容
- ✅ Dry-run 推迟到 Phase 5（不在 Phase 4 范围）

---

## Work Objectives

### Core Objective
让 GUI 成为 DLSS Compositor 的"无脑可用"入口：用户**最少点击数**完成配置（自动检测优先）、**实时可见**进度和预览、**清晰错误**反馈、**完整覆盖**所有 CLI 能力。同时把过程中发现的 CLI 痛点（auto-discovery、稳定输出格式）反向修复。

### Concrete Deliverables

**GUI 新功能**：
- Source FPS / Sharpness / NGX log level / Tonemap=custom 控件
- Target resolution preset (4K/1440p/1080p/Custom) + 实际 scale 双向显示
- Source resolution 自动检测（从 camera.json 的 render_width/render_height）
- camera.json 自动填充（选完 input-dir 后扫描）
- Live frame preview 真实图像（FFmpeg 转 PNG，渲染 <img>）
- NGX banner 显示（顶部状态栏：SDK 版本、SR/FG 可用性、preset 选择）
- ETA 估算（基于已用时间 + 进度）
- 表单结构化（advanced 默认折叠、新增 "Resolution" group）
- Validation 增强（cameraData 缺失提示更友好、source resolution 不匹配警告）

**CLI 反向改动**：
- `<input-dir>/camera.json` 自动发现（如 `--camera-data` 未指定）
- `--progress-format json` flag → NDJSON stdout 流
- 事件类型：`frame_started`, `frame_written`, `complete`, `error`, `banner`
- 默认仍是人读格式（向后兼容）

**新 IPC channels**：
- `frame:preview-png` — 请求 GUI 端 FFmpeg 转 PNG
- `camera:auto-detect` — 扫描目录返回 camera.json 路径
- `camera:read-resolution` — 读 camera.json 返回 render_w/h
- `process:ndjson-event` — 替代/补充现有 `process:progress`

**Tests**：
- Playwright spec for resolution linkage、camera auto-detect、live preview
- 单元测试 NDJSON parser
- C++ 单测覆盖 camera.json auto-discovery

**Docs**：
- README 更新 GUI 流程截图 + 关键步骤
- `docs/gui_guide.md` 新增

### Definition of Done
- [ ] 用户选 input-dir → camera.json 自动填、source res 自动算、target preset 推荐 4K → 一次点 Start 即可
- [ ] 进度栏每帧实时显示新 EXR 的 PNG 预览（不是文件名）
- [ ] 顶部状态栏显示 "DLSS SDK 310.5.3 | DLSS-SR ✅ | DLSS-FG ✅ (4x)"
- [ ] `--progress-format json` 输出可被 GUI 100% 正确解析（regex 退役）
- [ ] CLI `--input-dir foo/` 不传 `--camera-data` 时自动用 `foo/camera.json`（如存在）
- [ ] 全套 Playwright + ctest + pytest 通过

### Must Have
- 4 个新 CLI flag（`--source-fps`, `--sharpness`, `--ngx-log-level`, `--tonemap custom`）在 GUI 中暴露
- camera.json 自动发现（GUI + CLI 两端）
- Live frame preview 真实图像
- NDJSON 输出 + GUI 解析
- NGX banner 显示
- 所有现有 e2e 测试不退化

### Must NOT Have (Guardrails)
- ❌ 不引入新的 EXR 解码库到 GUI（只用 FFmpeg）
- ❌ 不破坏 CLI 默认人读输出格式（NDJSON 是 opt-in）
- ❌ 不删除现有 IPC channel（新增是允许的）
- ❌ 不改 CLI 核心 SR/FG 处理逻辑（只加 output 格式 + auto-detect）
- ❌ 不实现 `--dry-run`（推迟到 Phase 5）
- ❌ 不实现 GUI 内置 EXR decoder（FFmpeg 转 PNG 路径）
- ❌ 不重写 React Context state 架构（增量修改）
- ❌ 不引入 Redux/Zustand/MobX（保留现有 Context + useReducer）
- ❌ 不引入额外 npm 重依赖（heavy: WebGL 库、EXR-wasm 等）
- ❌ 不修改 SR/FG/EXR core 处理（与 GUI 无关）
- ❌ 不强制启用 NDJSON（默认人读、`--progress-format human` 显式标）

---

## Verification Strategy

### Test Decision
- **基础设施**: YES (Catch2, pytest, Playwright)
- **自动化测试**: YES (TDD for NDJSON parser, e2e for GUI flows)
- **C++ Framework**: Catch2
- **Python Framework**: pytest
- **TypeScript Framework**: Playwright + 单元测试可用 vitest/jest（如已配）

### QA Policy
- **CLI 改动**: ctest + Bash CLI 调用验证 NDJSON 输出正确性
- **GUI 改动**: Playwright e2e + 单元测试覆盖新组件
- **集成**: 完整跑一段真实序列，用 GUI 操作，验证全程顺畅

证据保存到 `.sisyphus/evidence/phase4/task-{N}-{slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave A (CLI 反向改动 — 必须先做，GUI 后续依赖):
├── Task 1: CLI camera.json auto-discovery
└── Task 2: CLI --progress-format json (NDJSON 输出)

Wave B (GUI 暴露已有 CLI flag — 独立小项):
├── Task 3: 新增 source-fps / sharpness / ngx-log-level 控件
└── Task 4: tonemap select 加 'custom' 选项

Wave C (GUI 资源/解析增强):
├── Task 5: NDJSON parser 替代 regex (依赖 Wave A.2)
├── Task 6: NGX banner 头部状态栏
└── Task 7: camera.json 自动检测 + source resolution 解析 (依赖 Wave A.1)

Wave D (GUI 核心 UX 升级):
├── Task 8: Target resolution + scale 双向联动 (依赖 Wave C.7)
├── Task 9: Live frame preview (FFmpeg PNG)
└── Task 10: ETA 估算 + 表单 UX polish

Wave E (测试 + 文档):
├── Task 11: Playwright e2e 覆盖新流程
└── Task 12: 文档更新

Wave FINAL (审查):
├── F1: 计划合规审计 (oracle)
├── F2: 代码质量审查 (unspecified-high)
├── F3: 真实 QA (unspecified-high)
└── F4: 范围保真度检查 (deep)
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 7, 11 | A |
| 2 | — | 5, 9, 11 | A |
| 3 | — | 11 | B |
| 4 | — | 11 | B |
| 5 | 2 | 9, 11 | C |
| 6 | — | 11 | C |
| 7 | 1 | 8, 11 | C |
| 8 | 7 | 11 | D |
| 9 | 2, 5 | 11 | D |
| 10 | — | 11 | D |
| 11 | 1-10 | 12 | E |
| 12 | 11 | F1-F4 | E |

### Agent Dispatch Summary

- **Wave A**: 2 tasks — T1 → `quick`, T2 → `unspecified-high`
- **Wave B**: 2 tasks — T3 → `visual-engineering`, T4 → `quick`
- **Wave C**: 3 tasks — T5 → `unspecified-high`, T6 → `visual-engineering`, T7 → `visual-engineering`
- **Wave D**: 3 tasks — T8 → `visual-engineering`, T9 → `visual-engineering`, T10 → `visual-engineering`
- **Wave E**: 2 tasks — T11 → `unspecified-high`, T12 → `writing`
- **FINAL**: F1 → `oracle`, F2-F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. CLI camera.json auto-discovery

  **What to do**:
  - **`src/cli/cli_parser.cpp`** 或 **`src/main.cpp`**:
    - 在 config validation 阶段（CLI 解析完毕、batch process 启动前）：
      - 如果 `config.cameraDataFile` 为空且 `config.inputDir` 非空：
        - 检查 `<inputDir>/camera.json` 是否存在
        - 如果存在：`config.cameraDataFile = "<inputDir>/camera.json"`，输出 info log `[camera] auto-discovered: <path>`
        - 如果不存在：保持空（不报错——可能用户根本不用 FG）
  - **不**改 NGX/FG 调用逻辑（cameraDataFile 后续流程已成熟）
  - **测试** (`tests/test_cli.cpp` 或新增 `tests/test_camera_autodetect.cpp`):
    - case 1: input-dir 含 camera.json → cameraDataFile 自动填
    - case 2: input-dir 无 camera.json → cameraDataFile 保持空
    - case 3: 显式 `--camera-data foo.json` + input-dir 也有 camera.json → 用户显式优先（不被覆盖）

  **Must NOT do**:
  - 不修改 sequence_processor 的 cameraData 加载逻辑
  - 不在 cameraData 缺失时报错（FG 不强制需要 cameraData）
  - 不删除 `--camera-data` flag（仍是显式 override 路径）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave A)
  **Blocked By**: None (Phases 0-3 完成后)

  **References**:
  - `src/cli/cli_parser.cpp` - 当前 CLI 解析
  - `src/cli/config.h` - Config struct
  - `src/main.cpp` - 调用顺序
  - `src/pipeline/sequence_processor.cpp` - 后续如何用 cameraDataFile

  **Acceptance Criteria**:
  - [ ] 包含 camera.json 的 input-dir → 自动用上
  - [ ] 不包含 → 保持空（不报错）
  - [ ] 显式指定 → 不被自动发现覆盖
  - [ ] info log 标注 "auto-discovered"

  **QA Scenarios**:
  ```
  Scenario: 自动发现 camera.json
    Tool: Bash
    Steps:
      1. 准备 fixture: tests/fixtures/with_camera_json/ 含 frame_*.exr 和 camera.json
      2. cmake --build build --config Release
      3. build\Release\dlss-compositor.exe --input-dir tests/fixtures/with_camera_json/ --output-dir tmp/ --interpolate 2x --ngx-log-level info 2>&1 | findstr /c:"auto-discovered"
    Expected Result: 输出 "[camera] auto-discovered: ..." 行；处理成功
    Evidence: .sisyphus/evidence/phase4/task-1-autodiscover.txt

  Scenario: 显式 override 不被覆盖
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --input-dir tests/fixtures/with_camera_json/ --camera-data tests/fixtures/other_camera.json --output-dir tmp/ --interpolate 2x 2>&1 | findstr /c:"auto-discovered"
    Expected Result: 0 匹配（不该有 auto-discover 日志）
    Evidence: .sisyphus/evidence/phase4/task-1-no-override.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): auto-discover camera.json from input directory`
  - Files: `src/cli/cli_parser.cpp 或 src/main.cpp, src/pipeline/sequence_processor.cpp (如需), tests/*`
  - Pre-commit: `ctest --test-dir build`

- [x] 2. CLI --progress-format json (NDJSON 输出)

  **What to do**:
  - **`src/cli/config.h`**:
    - 新增 `enum class ProgressFormat { Human, Json };`
    - 字段 `ProgressFormat progressFormat = ProgressFormat::Human;`
  - **`src/cli/cli_parser.cpp`**:
    - 新增 `--progress-format <human|json>` flag (默认 human)
    - `--help` 描述
  - **新建 `src/core/ndjson_emitter.h/cpp`**:
    ```cpp
    class NdjsonEmitter {
    public:
        // 单一公共方法：emit 一行 JSON 到 stdout（或 stderr）
        void emit(const nlohmann::json& event);
        bool isEnabled() const;
        void setEnabled(bool e);
        // ... thread-safe via mutex if multiple threads emit
    private:
        std::mutex m_mutex;
        bool m_enabled = false;
    };
    ```
    - 单例或全局实例（在 main.cpp 初始化）
    - 写到 stdout，每条独立一行（NDJSON）
    - 必须线程安全（async writer 可能从工作线程调用）
  - **`src/pipeline/sequence_processor.cpp`** 在以下位置发射事件：
    - 处理开始：`{"event":"started","total":N,"input_dir":"...","output_dir":"...","scale":2.0,"interpolate":2}`
    - 每帧开始：`{"event":"frame_started","n":42,"total":100,"timestamp_ms":...}`
    - 每帧 EXR 写完：`{"event":"frame_written","n":42,"path":"out/frame_0042.exr","timestamp_ms":...}`（async writer 完成回调里发）
    - 错误：`{"event":"error","frame":42,"message":"..."}`
    - 完成：`{"event":"complete","total":100,"duration_ms":...,"errors":0}`
  - **`src/dlss/ngx_wrapper.cpp`** banner 也加 NDJSON 版本（可选）：
    - `{"event":"banner","sdk":"310.5.3","engine":"dlss-compositor 0.2.1","sr_available":true,"fg_available":true,"max_multi_frame":4}`
    - banner 始终发（无论 progress format），因为 GUI 头部要展示
  - **当 `--progress-format human` 时**: 完全保持现状（向后兼容）
  - **当 `--progress-format json` 时**: 不打印 "Processing frame N/M" 等人读行，改用 NDJSON
  - **Tests** (`tests/test_ndjson_emitter.cpp`):
    - emit 一个 event → stdout 捕获 → JSON 解析校验
    - 多线程并发 emit → 行不会交错
    - isEnabled=false → 不输出

  **Must NOT do**:
  - 不破坏 human format 默认行为
  - 不在 NDJSON 模式输出非 JSON 行到 stdout（stderr 可继续 NGX log 等）
  - 不引入新 JSON 库（用现有 nlohmann::json）
  - 不强制 GUI 必须用 NDJSON（GUI 端 Wave C 改）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**: YES (Wave A)
  **Blocked By**: None

  **References**:
  - `src/cli/config.h` - 已有 enum 模式
  - `src/pipeline/sequence_processor.cpp:600+` - 帧处理循环
  - `src/pipeline/async_exr_writer.cpp` - 异步写入，需要在 write 完成回调里 emit
  - nlohmann::json - 项目已用（camera_data_loader.cpp 引用过）

  **Acceptance Criteria**:
  - [ ] `--progress-format json` 输出每行都是合法 JSON
  - [ ] `frame_written` 事件路径正确指向真实写入文件
  - [ ] 多线程 emit 不交错（行原子性）
  - [ ] 默认 human 格式无 NDJSON 输出
  - [ ] 单元测试覆盖 emitter

  **QA Scenarios**:
  ```
  Scenario: NDJSON 输出格式正确
    Tool: Bash + Python
    Steps:
      1. build\Release\dlss-compositor.exe --progress-format json --input-dir tests/fixtures/sequence/ --output-dir tmp/ --scale 2 > out.ndjson 2> err.log
      2. python -c "import json,sys; [json.loads(l) for l in open('out.ndjson') if l.strip()]"  -- 不抛异常
      3. python -c "import json; lines=[json.loads(l) for l in open('out.ndjson') if l.strip()]; print([e['event'] for e in lines])"  -- 应有 ['started', 'frame_started', 'frame_written', ..., 'complete']
    Expected Result: 全部行解析成功；事件类型完整
    Evidence: .sisyphus/evidence/phase4/task-2-ndjson.txt

  Scenario: human 格式向后兼容
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir tmp/ --scale 2 2>&1 | findstr /c:"Processing frame"
    Expected Result: 至少 1 行匹配（人读格式仍工作）
    Evidence: .sisyphus/evidence/phase4/task-2-human-compat.txt

  Scenario: 单元测试
    Tool: ctest
    Steps:
      1. ctest --test-dir build -R ndjson
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-2-unit.txt
  ```

  **Commit**: YES
  - Message: `feat(cli): add --progress-format json with NDJSON event stream`
  - Files: `src/cli/*, src/core/ndjson_emitter.*, src/pipeline/sequence_processor.cpp, src/pipeline/async_exr_writer.cpp, src/dlss/ngx_wrapper.cpp, tests/test_ndjson_emitter.cpp`
  - Pre-commit: `ctest`

- [x] 3. GUI: source-fps / sharpness / ngx-log-level 控件

  **What to do**:
  - **`gui/src/types/dlss-config.ts`**:
    - 扩展 `DlssConfig` interface:
      ```typescript
      sourceFps: number;          // default 30
      sharpness: number;          // default 0.0, range [0.0, 1.0]
      ngxLogLevel: 'off' | 'error' | 'warn' | 'info' | 'verbose'; // default 'warn'
      ```
    - 更新 default config 对象
  - **`gui/src/state/config-store.ts`**:
    - 加 dispatch action: `SET_SOURCE_FPS`, `SET_SHARPNESS`, `SET_NGX_LOG_LEVEL`
    - reducer 处理新 action
  - **`gui/src/components/BasicSettings.tsx`**:
    - 新增 "Source FPS" 数字输入（与现有 FPS 区分清楚——后者是视频编码 FPS）
    - 推荐位置：在 Quality / Preset 之后，FPS 之前
    - 标签 "Source FPS (animation frame rate)"
    - tooltip 说明区别
    - 范围 1-240，默认 30，clamp on blur
  - **`gui/src/components/AdvancedSettings.tsx`**:
    - 新增 "Sharpness" slider [0.0, 1.0]，default 0.0
    - 标签提示："Post-DLSS sharpening (0=off, 0.2-0.4 recommended)"
    - 新增 "NGX Log Level" select: off/error/warn/info/verbose（默认 warn）
    - 标签 "NGX SDK Log Verbosity"
  - **`gui/electron/cli-args.ts`**:
    - buildCliArgs 加：
      ```typescript
      args.push('--source-fps', String(config.sourceFps));
      if (config.sharpness > 0) args.push('--sharpness', String(config.sharpness));
      if (config.ngxLogLevel !== 'warn') args.push('--ngx-log-level', config.ngxLogLevel);
      ```
  - **`gui/electron/settings-store.ts`**:
    - schema 加新字段，默认值如上
  - **`gui/src/App.tsx`**:
    - settings load/save 流程加新字段映射
  - 注意：UI 风格与现有控件一致（Tailwind class、错误处理、disabled state）

  **Must NOT do**:
  - 不修改 source-fps/sharpness/ngx-log-level 在 CLI 端的语义
  - 不与现有 FPS（视频编码）控件混淆
  - 不引入新 npm 依赖

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: YES (Wave B)
  **Blocked By**: None (Phases 1-3 完成提供 CLI 端)

  **References**:
  - `gui/src/components/BasicSettings.tsx:164-182` 现有 FPS 控件作模板
  - `gui/src/components/AdvancedSettings.tsx:76-107` Memory budget slider 作模板
  - `gui/electron/cli-args.ts:3-67` 现有 args builder
  - Phase 1 已在 CLI 加好对应 flag

  **Acceptance Criteria**:
  - [ ] 3 个新控件出现在对应面板
  - [ ] 默认值正确（30 / 0.0 / warn）
  - [ ] CLI args 正确生成
  - [ ] 持久化跨会话保持

  **QA Scenarios**:
  ```
  Scenario: 控件渲染 + 默认值
    Tool: Playwright
    Steps:
      1. cd gui; npx playwright test tests/new-controls.spec.ts
      2. 验证: source-fps 默认显示 30; sharpness slider 在 0; ngx-log-level select 默认 warn
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-3-controls.txt

  Scenario: CLI args 反映 GUI 状态
    Tool: Playwright + spawn mock
    Steps:
      1. 设置 source-fps=24, sharpness=0.3, ngx-log-level=info
      2. 调用 buildCliArgs（unit test 或 spy）
      3. 检查 args 数组含 ['--source-fps','24','--sharpness','0.3','--ngx-log-level','info']
    Expected Result: 完整匹配
    Evidence: .sisyphus/evidence/phase4/task-3-args.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): expose --source-fps, --sharpness, --ngx-log-level controls`
  - Files: `gui/src/types/dlss-config.ts, gui/src/state/config-store.ts, gui/src/components/BasicSettings.tsx, gui/src/components/AdvancedSettings.tsx, gui/electron/cli-args.ts, gui/electron/settings-store.ts, gui/src/App.tsx`
  - Pre-commit: `cd gui; npm run build && npx playwright test tests/new-controls.spec.ts`

- [x] 4. GUI: tonemap select 加 'custom' 选项

  **What to do**:
  - **`gui/src/types/dlss-config.ts`**:
    - `tonemapMode` 类型扩展为 `'none' | 'pq' | 'custom'`
  - **`gui/src/components/AdvancedSettings.tsx:208-217`**:
    - tonemap select 新增 `<option value="custom">Custom (LUT files)</option>`
  - **LUT 文件 picker 启用条件更新** (AdvancedSettings.tsx:241, 264):
    - Forward LUT picker enabled if `tonemapMode === 'custom'` (之前是 'pq')
    - Inverse LUT picker enabled if `tonemapMode === 'custom' && inverseTonemapEnabled`
    - 文案更新：从 "PQ tonemap LUT" → "Custom tonemap LUT"
  - **`gui/electron/cli-args.ts`**:
    - 加 `if (config.tonemapMode === 'custom') args.push('--tonemap','custom');`
    - 'pq' 仍是默认（不显式传，CLI 端 default 已是 PQ）
    - 'none' 显式传 `--tonemap none`
  - **Validation**:
    - 当 `tonemapMode === 'custom'` 时，Forward LUT 文件**必填**
    - Start 按钮在 custom + LUT 缺失时 disabled
    - 错误提示明确："Custom tonemap requires forward LUT file"

  **Must NOT do**:
  - 不修改 PQ 默认行为
  - 不删除 'none' 选项
  - 不强制 inverse LUT（forward 必填，inverse 可选）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave B)
  **Blocked By**: Phase 1 Task 2（CLI 已能解析 custom）

  **References**:
  - `gui/src/components/AdvancedSettings.tsx:208-275` tonemap + LUT 控件
  - `gui/electron/cli-args.ts` args builder
  - Phase 3 Task 3 LUT 生成器（用户可生成 .bin 文件）

  **Acceptance Criteria**:
  - [ ] tonemap select 三个选项：none / pq / custom
  - [ ] custom 模式下 LUT picker enabled
  - [ ] Start 按钮在 custom + 无 forward LUT 时 disabled

  **QA Scenarios**:
  ```
  Scenario: custom 选项 + 验证
    Tool: Playwright
    Steps:
      1. 选 tonemap=custom，不选 forward LUT
      2. 检查 Start 按钮 disabled
      3. 选 forward LUT
      4. 检查 Start 按钮 enabled
    Expected Result: 行为符合预期
    Evidence: .sisyphus/evidence/phase4/task-4-custom-tonemap.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): add 'custom' tonemap mode option with LUT file requirement`
  - Files: `gui/src/types/dlss-config.ts, gui/src/components/AdvancedSettings.tsx, gui/electron/cli-args.ts, gui/src/components/ProcessingView.tsx (validation)`
  - Pre-commit: `cd gui; npm run build`

- [x] 5. GUI: NDJSON parser 替代 regex

  **What to do**:
  - **`gui/electron/cli-args.ts`**:
    - buildCliArgs **始终**追加 `args.push('--progress-format', 'json');`（GUI 一律用 NDJSON）
  - **`gui/electron/progress-parser.ts`**:
    - 重写 `parseProgressLine`：尝试 `JSON.parse(line)`；失败则视为非进度行（forward to log）
    - 新增 event-typed parser:
      ```typescript
      export type ProgressEvent =
        | { event: 'started'; total: number; ... }
        | { event: 'frame_started'; n: number; total: number; timestamp_ms: number }
        | { event: 'frame_written'; n: number; path: string; timestamp_ms: number }
        | { event: 'banner'; sdk: string; engine: string; sr_available: boolean; fg_available: boolean; max_multi_frame: number }
        | { event: 'error'; frame?: number; message: string }
        | { event: 'complete'; total: number; duration_ms: number; errors: number };
      
      export function parseNdjsonLine(line: string): ProgressEvent | null { ... }
      ```
    - 保留 `ProgressLineBuffer` (line splitter 不变)
  - **`gui/electron/main.ts:104-116`** stdout/stderr 处理：
    - 每行用 `parseNdjsonLine` 解析
    - 根据 event 类型路由到不同 IPC：
      - `frame_started`/`complete` → `PROCESS_PROGRESS` (兼容现有)
      - `frame_written` → 新 IPC `PROCESS_FRAME_WRITTEN`
      - `banner` → 新 IPC `PROCESS_BANNER`
      - `error` → `PROCESS_ERROR`
      - 解析失败的行 → `PROCESS_LOG`（视为普通日志）
  - **`gui/electron/ipc-channels.ts` + `gui/src/ipc/channels.ts`**:
    - 新增常量 `PROCESS_FRAME_WRITTEN`, `PROCESS_BANNER`
  - **`gui/electron/preload.ts`** + **`gui/src/types/window.d.ts`**:
    - 暴露 `onFrameWritten`, `onBanner` 监听器
  - **`gui/src/hooks/useProcessing.ts`**:
    - 注册 `onFrameWritten` → 更新 `latestFramePath` state（替代之前的 3s 轮询）
    - 注册 `onBanner` → 存到 store / 顶部状态栏（Wave C Task 6 用）
  - **删除 `frame:latest` IPC 轮询**（保留 IPC handler 作为 fallback，但不在 useProcessing 中调用）
  - **Tests** (`gui/tests/ndjson-parser.spec.ts` 或 vitest 单测):
    - 各 event 类型解析正确
    - 非 JSON 行返回 null
    - 部分 JSON 行不崩溃

  **Must NOT do**:
  - 不删除现有 `PROCESS_PROGRESS` IPC（保持向后兼容，frame_started 仍发它）
  - 不删除 `frame:latest` IPC handler（fallback path）
  - 不假设 NDJSON 行总能解析（log 行混杂时优雅降级）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**: YES (Wave C)
  **Blocked By**: Wave A Task 2（CLI NDJSON 输出）

  **References**:
  - `gui/electron/progress-parser.ts:5-42` 现有正则解析
  - `gui/electron/main.ts:72-130` IPC routing
  - `gui/src/hooks/useProcessing.ts` 现有进度处理

  **Acceptance Criteria**:
  - [ ] GUI 启动 spawn 时始终带 `--progress-format json`
  - [ ] frame_started 事件 → 进度条更新
  - [ ] frame_written 事件 → latestFramePath 立即更新（无 3s 延迟）
  - [ ] banner 事件 → 头部状态栏（Task 6 完成后）
  - [ ] 单元测试覆盖

  **QA Scenarios**:
  ```
  Scenario: NDJSON 解析正确
    Tool: Playwright + Node assertions
    Steps:
      1. cd gui; npx playwright test tests/ndjson-parser.spec.ts
      2. 模拟 stdout 输入各种事件，验证 IPC 派发正确
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-5-ndjson-parser.txt

  Scenario: 真实端到端
    Tool: Playwright e2e
    Steps:
      1. 跑真实序列处理
      2. 抓取 IPC 事件流
      3. 验证 frame_started / frame_written / complete 都收到
    Expected Result: 事件流完整
    Evidence: .sisyphus/evidence/phase4/task-5-e2e.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): use NDJSON progress events (replace regex parser)`
  - Files: `gui/electron/progress-parser.ts, gui/electron/main.ts, gui/electron/cli-args.ts, gui/electron/ipc-channels.ts, gui/src/ipc/channels.ts, gui/electron/preload.ts, gui/src/types/window.d.ts, gui/src/hooks/useProcessing.ts, gui/tests/ndjson-parser.spec.ts`
  - Pre-commit: `cd gui; npm run build && npx playwright test`

- [x] 6. GUI: NGX banner 头部状态栏

  **What to do**:
  - **新建 `gui/src/components/StatusBar.tsx`**:
    - 顶部固定一条状态栏组件
    - 显示：
      - DLSS SDK 版本（如 "DLSS SDK 310.5.3"）
      - DLSS-SR 可用性（绿色 ✅ "DLSS-SR" 或 红色 ❌ "DLSS-SR unavailable"）
      - DLSS-FG 可用性 + max-multi-frame（"DLSS-FG (max 4x)"）
      - 当前 dlss-compositor.exe 版本（如 "Engine 0.2.1"）
    - 数据源：`window.dlssApi.getBanner()` 或 store.banner（来自 Task 5 的 onBanner 监听）
    - 启动时如果未收到 banner，显示 "Initializing..." 或 "Click Start to detect"
  - **`gui/src/state/banner-store.ts`** (新建，简单 store):
    - 缓存 banner 数据；onBanner 回调时更新
  - **`gui/src/App.tsx`**:
    - 顶部 mount StatusBar
  - **触发 banner 输出**:
    - 现状：banner 仅在 NgxContext::init 时输出（处理任务一开始）
    - 改进：GUI 首次启动时主动跑一次 `dlss-compositor.exe --test-ngx --progress-format json` (无 input/output dir)，仅获取 banner 后退出
    - 这样无需用户启动处理就能看到 NGX 可用性
    - 实现：electron/main.ts 应用启动后 spawn 一次 test-ngx；解析 banner；推送到 renderer
  - **错误状态展示**:
    - 如果 DLSS-SR/FG 都 unavailable → 状态栏红字警告，主面板显示 "DLSS not available, processing will fail"
    - 失败原因（NGX banner 中的 unavailable_reason）展示

  **Must NOT do**:
  - 不阻塞 GUI 启动等 banner（异步）
  - 不假设 dlss-compositor.exe 总能找到（exe 不存在时 StatusBar 显示 "Configure exe path first"）
  - 不引入新 UI 库（用现有 Tailwind）

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: YES (Wave C)
  **Blocked By**: None (Phase 2 Task 4 已加好 banner 输出；Wave C Task 5 解析 banner 事件——可并行做但建议 Task 5 先 ready)

  **References**:
  - Phase 2 Task 4 banner 格式（`[NGX]` 前缀的人读 + `banner` event NDJSON）
  - `gui/src/components/ExePathConfig.tsx` 作为类似"状态显示"组件参考
  - `gui/src/components/Layout.tsx` 当前 layout 结构

  **Acceptance Criteria**:
  - [ ] StatusBar 在 app 顶部可见
  - [ ] 启动后 2-3 秒内显示 NGX 信息
  - [ ] DLSS 不可用时红字警告
  - [ ] exe 路径未配时显示对应提示

  **QA Scenarios**:
  ```
  Scenario: StatusBar 显示
    Tool: Playwright
    Steps:
      1. 启动 GUI（exe path 已配）
      2. 等待 5 秒
      3. 检查 StatusBar 含 "DLSS SDK", "Engine", "DLSS-SR" 文字
    Expected Result: 全部出现
    Evidence: .sisyphus/evidence/phase4/task-6-statusbar.png

  Scenario: exe 缺失时降级
    Tool: Playwright
    Steps:
      1. 清空 exePath setting
      2. 启动 GUI
      3. StatusBar 显示 "Configure exe path"
    Expected Result: 友好降级
    Evidence: .sisyphus/evidence/phase4/task-6-no-exe.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): NGX banner status bar (SDK version, DLSS-SR/FG availability)`
  - Files: `gui/src/components/StatusBar.tsx (new), gui/src/state/banner-store.ts (new), gui/electron/main.ts (test-ngx spawn), gui/src/App.tsx`
  - Pre-commit: `cd gui; npm run build && npx playwright test tests/statusbar.spec.ts`

- [x] 7. GUI: camera.json 自动检测 + source resolution 解析

  **What to do**:
  - **新建 `gui/electron/camera-detector.ts`**:
    ```typescript
    import * as fs from 'fs';
    import * as path from 'path';
    
    export function findCameraJson(inputDir: string): string | null {
        const candidate = path.join(inputDir, 'camera.json');
        return fs.existsSync(candidate) ? candidate : null;
    }
    
    export function readSourceResolution(cameraJsonPath: string): { width: number; height: number } | null {
        try {
            const data = JSON.parse(fs.readFileSync(cameraJsonPath, 'utf-8'));
            const w = data.render_width;
            const h = data.render_height;
            if (typeof w === 'number' && typeof h === 'number' && w > 0 && h > 0) {
                return { width: w, height: h };
            }
        } catch (e) {
            return null;
        }
        return null;
    }
    ```
  - **`gui/electron/ipc-channels.ts`** + **`gui/src/ipc/channels.ts`**:
    - 新增 `CAMERA_AUTO_DETECT = 'camera:auto-detect'`
  - **`gui/electron/main.ts`**:
    - 注册 `ipcMain.handle('camera:auto-detect', async (_, inputDir: string) => { ... })`:
      - 调用 `findCameraJson` + `readSourceResolution`
      - 返回 `{ cameraJsonPath: string | null, sourceWidth: number | null, sourceHeight: number | null }`
  - **`gui/electron/preload.ts`** + **`gui/src/types/window.d.ts`**:
    - 暴露 `cameraAutoDetect: (inputDir: string) => Promise<{...}>`
  - **`gui/src/components/InputOutputPanel.tsx:11-20`** handleInputDir:
    - 选完目录后调用 `await window.dlssApi.cameraAutoDetect(dir)`
    - 如果返回 cameraJsonPath：
      - dispatch SET_CAMERA_DATA_FILE → 自动填到 camera.json 输入框
      - 提示 "Auto-filled camera.json from input directory"
    - 如果返回 sourceWidth/Height：
      - dispatch SET_SOURCE_RESOLUTION（新 action）→ 存到 config-store
      - Task 8 会用这个数据
  - **`gui/src/state/config-store.ts`** + **`gui/src/types/dlss-config.ts`**:
    - 新增 state: `sourceWidth: number | null = null`, `sourceHeight: number | null = null`
    - 新增 actions: `SET_SOURCE_RESOLUTION`
  - **UI 提示**:
    - InputOutputPanel 在自动填后显示一行小字 "✓ Detected camera.json (1920×1080 @ 30fps)"
    - camera.json picker 旁边显示 "Auto" 标签（如果是自动填的）

  **Must NOT do**:
  - 不强制用户接受自动检测（仍可手动覆盖）
  - 不在 camera.json 不存在时报错（仅 silently skip）
  - 不读 jitter / frames 等大字段（仅 render_width/height）
  - 不 watch 目录（只在 selectDirectory 后扫一次）

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: YES (Wave C)
  **Blocked By**: Wave A Task 1（CLI 端也能 auto-discover 让两端行为一致）

  **References**:
  - `gui/src/components/InputOutputPanel.tsx:11-31` 当前 handle 函数
  - `src/core/camera_data_loader.h` JSON 字段定义（render_width/render_height/version 等）
  - `tests/fixtures/camera.json` 真实样例

  **Acceptance Criteria**:
  - [ ] 选含 camera.json 的目录后自动填到 picker
  - [ ] 检测到 source resolution 存到 store
  - [ ] camera.json 缺失时静默
  - [ ] 用户可手动覆盖

  **QA Scenarios**:
  ```
  Scenario: 自动填 camera.json
    Tool: Playwright
    Steps:
      1. 启动 GUI
      2. 选 input-dir = tests/fixtures/with_camera_json/
      3. 等 1 秒
      4. 检查 cameraDataFile 输入框已填路径
      5. 检查提示文字 "Auto-filled" 或 "Detected"
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-7-autofill.txt

  Scenario: 静默处理缺失
    Tool: Playwright
    Steps:
      1. 选 input-dir 不含 camera.json
      2. 检查 cameraDataFile 仍为空，无错误提示
    Expected Result: 静默
    Evidence: .sisyphus/evidence/phase4/task-7-silent.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): auto-discover camera.json + source resolution detection`
  - Files: `gui/electron/camera-detector.ts (new), gui/electron/main.ts, gui/electron/ipc-channels.ts, gui/electron/preload.ts, gui/src/ipc/channels.ts, gui/src/types/window.d.ts, gui/src/state/config-store.ts, gui/src/types/dlss-config.ts, gui/src/components/InputOutputPanel.tsx`
  - Pre-commit: `cd gui; npm run build && npx playwright test tests/auto-detect.spec.ts`

- [x] 8. GUI: Target resolution + scale 双向联动

  **What to do**:
  - **新建 `gui/src/components/ResolutionPanel.tsx`**:
    - 设计：一个面板，左右双列
      - 左列：source resolution（read-only，从 store.sourceWidth/Height 显示，e.g., "1920 × 1080"）
      - 中间：箭头 →
      - 右列：target resolution（用户控制，preset dropdown + custom WxH 输入）
      - 下方：scale 显示（read-only，自动计算 = target_w / source_w）
    - Target preset dropdown:
      - "4K (3840 × 2160)"
      - "1440p (2560 × 1440)"
      - "1080p (1920 × 1080)"
      - "Custom..." → 展开 width/height 数字输入
      - "Same as source (DLAA)" → target = source, scale = 1.0
    - 当 source 未知（无 camera.json）时：
      - source 列显示 "Unknown" + 一个手动输入框让用户填 source resolution
      - 或者 fallback 到旧版 scale-only 输入
  - **`gui/src/state/config-store.ts`**:
    - 新增 actions: `SET_TARGET_RESOLUTION_PRESET`, `SET_CUSTOM_TARGET_RESOLUTION`
    - reducer: 当 target 或 source 改变 → 重算 `scaleFactor = target_w / source_w`
    - 仍维护 `scaleFactor` 字段（与 CLI 端兼容；ResolutionPanel 是上层抽象）
  - **`gui/src/components/BasicSettings.tsx`**:
    - 移除原本的 scale 数字输入（被 ResolutionPanel 替代）
    - 或保留作为 "Advanced" 折叠下的覆盖（用户偏要直接输入 1.5 也行）
  - **`gui/src/App.tsx`**:
    - 在 BasicSettings 上方挂 `<ResolutionPanel />`
  - **CLI args**: 不变（仍发 `--scale X.XX`，CLI 端无感知）
  - **回归 fallback**:
    - 如果 source unknown 且 user 不输入 → 显示警告 "Source resolution unknown, scale-based mode"，回退到旧版直接 scale 输入

  **Must NOT do**:
  - 不修改 CLI args（`--scale` 仍是唯一传给 CLI 的方式）
  - 不假设 source 一定可知（必须有 fallback）
  - 不强制用户用 preset（custom 数字输入仍可）
  - 不引入复杂 dropdown 库

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: NO (Wave D)
  **Blocked By**: Wave C Task 7（需 sourceWidth/Height in store）

  **References**:
  - `gui/src/components/BasicSettings.tsx:50-70` 现有 scale 输入
  - Tailwind grid layout
  - state from Task 7

  **Acceptance Criteria**:
  - [ ] source 已知时双列显示
  - [ ] preset 切换正确算 scale
  - [ ] custom 模式手动输入 WxH
  - [ ] source 未知时 fallback 到 scale 输入
  - [ ] CLI args 中 scale 正确反映用户选择

  **QA Scenarios**:
  ```
  Scenario: source 已知 → 4K preset → scale=2.0
    Tool: Playwright
    Steps:
      1. 选 input-dir 含 camera.json (1920×1080 source)
      2. ResolutionPanel 显示 source 1920×1080
      3. 选 target preset "4K"
      4. 检查 scale 显示 2.0
      5. 启动处理，spawn args 含 "--scale 2"
    Expected Result: 全链路正确
    Evidence: .sisyphus/evidence/phase4/task-8-4k-preset.txt

  Scenario: source 未知 → fallback
    Tool: Playwright
    Steps:
      1. 选 input-dir 不含 camera.json
      2. ResolutionPanel 显示 "Source unknown" + 手动 source 输入框
      3. 或显示直接 scale 输入
    Expected Result: 优雅 fallback
    Evidence: .sisyphus/evidence/phase4/task-8-fallback.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): target resolution + scale linkage (4K/1440p/1080p preset)`
  - Files: `gui/src/components/ResolutionPanel.tsx (new), gui/src/components/BasicSettings.tsx, gui/src/state/config-store.ts, gui/src/App.tsx`
  - Pre-commit: `cd gui; npm run build && npx playwright test tests/resolution.spec.ts`

- [ ] 9. GUI: Live frame preview (FFmpeg PNG)

  **What to do**:
  - **新建 `gui/electron/preview-generator.ts`**:
    ```typescript
    import { spawn } from 'child_process';
    import * as path from 'path';
    import * as os from 'os';
    import * as fs from 'fs';
    
    // FFmpeg 转 EXR → 缩略 PNG
    export async function exrToPng(exrPath: string, maxDim: number = 480): Promise<string> {
        const tmpPng = path.join(os.tmpdir(), `dlss-preview-${path.basename(exrPath)}.png`);
        return new Promise((resolve, reject) => {
            const args = [
                '-y',
                '-i', exrPath,
                '-vf', `scale=${maxDim}:-1:flags=lanczos`,
                '-frames:v', '1',
                tmpPng
            ];
            const proc = spawn('ffmpeg', args, { stdio: 'pipe' });
            proc.on('close', (code) => {
                if (code === 0 && fs.existsSync(tmpPng)) resolve(tmpPng);
                else reject(new Error(`ffmpeg exit ${code}`));
            });
            proc.on('error', reject);
        });
    }
    ```
    - 缓存逻辑：相同 EXR path → 复用之前的 PNG（避免重复转换）
    - 清理：app 退出时删除临时 PNG
  - **`gui/electron/ipc-channels.ts`** + **`gui/src/ipc/channels.ts`**:
    - 新增 `FRAME_PREVIEW_PNG = 'frame:preview-png'`
  - **`gui/electron/main.ts`**:
    - `ipcMain.handle('frame:preview-png', async (_, exrPath) => exrToPng(exrPath))`
    - 错误时返回 null（GUI 显示 placeholder）
  - **`gui/electron/preload.ts`** + **`gui/src/types/window.d.ts`**:
    - 暴露 `getFramePreview: (exrPath: string) => Promise<string | null>`
  - **`gui/src/components/ProcessingView.tsx:229-256`** 重写 frame preview 区域:
    - 用 `latestFramePath`（来自 Task 5 的 onFrameWritten）
    - 当 latestFramePath 变化时，触发 `useEffect`:
      ```typescript
      useEffect(() => {
          if (!latestFramePath) return;
          window.dlssApi.getFramePreview(latestFramePath).then(setPreviewPng);
      }, [latestFramePath]);
      ```
    - 渲染：
      ```tsx
      {previewPng ? (
          <img src={`file://${previewPng}`} className="max-w-full rounded shadow-lg" alt="Latest frame" />
      ) : (
          <div className="text-gray-500">Waiting for first frame...</div>
      )}
      <div className="text-xs text-gray-500 mt-2">Frame {currentFrame}/{totalFrames}</div>
      ```
  - **FFmpeg 检测**:
    - app 启动时检测 ffmpeg 是否在 PATH
    - 不在则 preview 显示 fallback "FFmpeg not found, install for live preview"
    - 不阻塞处理（preview 是装饰性功能）
  - **临时文件管理**:
    - 用唯一文件名（含 EXR basename + hash）避免冲突
    - app 退出时清理 `dlss-preview-*` tmp 文件
    - 内存中限缓存 N 个最新 PNG path（默认 10）
  - **性能**:
    - FFmpeg 调用是异步，不阻塞 UI
    - 4K EXR 转 480p PNG 约 200-500ms
    - 帧率高时（< 1s/帧）会跳帧——可接受（preview 不是 every frame）
  - **节流**:
    - 如果 FFmpeg 还在转上一帧 → 跳过新的 frame_written（始终保持最新一个排队）

  **Must NOT do**:
  - 不引入 EXR-wasm 或 OpenEXR Node 库
  - 不在 renderer 端做图像解码（隔 Electron security boundary）
  - 不阻塞处理（preview 失败不影响主流程）
  - 不持久化 PNG（用 tmp 即可）

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: NO (Wave D)
  **Blocked By**: Wave A Task 2（NDJSON），Wave C Task 5（GUI 解析 frame_written）

  **References**:
  - FFmpeg CLI（项目已要求作为 video encoding 依赖）
  - `gui/src/components/ProcessingView.tsx:229-256` 当前 placeholder 实现
  - Node child_process spawn

  **Acceptance Criteria**:
  - [ ] FFmpeg 可调用且生成 PNG
  - [ ] PNG 在 GUI 中显示
  - [ ] 每收到 frame_written 事件后 1-2 秒内 preview 更新
  - [ ] FFmpeg 不在 PATH 时优雅降级
  - [ ] App 退出时清理 tmp PNG

  **QA Scenarios**:
  ```
  Scenario: FFmpeg 转 PNG 工作
    Tool: Playwright + Manual
    Steps:
      1. 启动 GUI，选 input/output dir
      2. 启动处理一段 5 帧序列
      3. 观察 preview 区域，前 10 秒应至少显示 1 张 PNG
    Expected Result: <img> 元素 src 指向 file://...png 且渲染成功
    Evidence: .sisyphus/evidence/phase4/task-9-preview.png

  Scenario: FFmpeg 缺失降级
    Tool: Playwright + 临时 mock
    Steps:
      1. 用 stub 让 FFmpeg spawn 失败
      2. 验证 preview 显示 fallback 文本
      3. 处理仍能完成
    Expected Result: 优雅降级
    Evidence: .sisyphus/evidence/phase4/task-9-no-ffmpeg.txt

  Scenario: 节流行为
    Tool: Playwright
    Steps:
      1. 模拟连续 frame_written 事件每 100ms 一次
      2. 验证 FFmpeg spawn 不重叠（跳帧）
    Expected Result: 无并发 spawn
    Evidence: .sisyphus/evidence/phase4/task-9-throttle.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): live frame preview via FFmpeg PNG conversion`
  - Files: `gui/electron/preview-generator.ts (new), gui/electron/main.ts, gui/electron/ipc-channels.ts, gui/electron/preload.ts, gui/src/ipc/channels.ts, gui/src/types/window.d.ts, gui/src/components/ProcessingView.tsx`
  - Pre-commit: `cd gui; npm run build && npx playwright test tests/preview.spec.ts`

- [ ] 10. GUI: ETA 估算 + 表单 UX polish

  **What to do**:
  - **`gui/src/hooks/useProcessing.ts`**:
    - 新 state: `etaSeconds: number | null`
    - 计算逻辑：
      ```typescript
      // 基于已处理帧数和已用时间外推
      const elapsedMs = Date.now() - startTime;
      const avgMsPerFrame = elapsedMs / currentFrame;
      const remainingFrames = totalFrames - currentFrame;
      const etaMs = avgMsPerFrame * remainingFrames;
      etaSeconds = Math.round(etaMs / 1000);
      ```
    - 更新触发：每个 frame_started 事件
    - 平滑：用滑动窗口（最近 N 帧均值）避免抖动
    - 边界：currentFrame < 3 时显示 "Estimating..."
  - **`gui/src/components/ProcessingView.tsx`**:
    - 在进度条下方新增一行：
      ```tsx
      <div className="text-xs text-gray-500 mt-1">
          {etaSeconds !== null ? `~${formatDuration(etaSeconds)} remaining` : 'Estimating...'}
      </div>
      ```
    - 进度条本身样式调整：更明显的颜色、动画
  - **表单 UX 优化**：
    - **AdvancedSettings 默认折叠**：用户首次见到只是 input/output + scale + quality，避免选项焦虑
    - **错误提示位置**：从字段下方改成 Start 按钮上方一个 banner，集中显示所有阻塞条件
    - **配色优化**：成功/警告/错误用统一颜色（Tailwind 标准 green-500/yellow-500/red-500）
    - **disabled state 可见性**：disabled 控件加 cursor-not-allowed 和淡色处理
    - **Tooltip 增强**：source-fps、sharpness、preset 这些不那么直观的字段加 hover tooltip
  - **`gui/src/components/InputOutputPanel.tsx`** UX 改进：
    - 选完目录后立即显示"找到 N 个 .exr 文件"反馈
    - 通过 IPC 调用 `dialog:openDirectory` 后顺便扫一下目录数
  - **新增 IPC**: `count:exr-files` 返回目录中 .exr 数量

  **Must NOT do**:
  - 不引入复杂 charting/animation 库
  - 不强制用户必须填所有字段（advanced 仍是可选）
  - 不改变核心交互流程（用户不需要重新学）

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: YES (Wave D)
  **Blocked By**: None (独立 UX 改进)

  **References**:
  - `gui/src/components/ProcessingView.tsx` 现有进度区域
  - `gui/src/hooks/useProcessing.ts` 进度状态
  - 通用 Tailwind UX 模式

  **Acceptance Criteria**:
  - [ ] ETA 在处理 3 帧后开始显示
  - [ ] AdvancedSettings 默认折叠
  - [ ] 错误信息集中在 Start 按钮上方
  - [ ] 选目录后显示文件数

  **QA Scenarios**:
  ```
  Scenario: ETA 显示
    Tool: Playwright
    Steps:
      1. 启动处理一段 ≥10 帧的序列
      2. 处理 3 帧后检查 ETA 区域有数字
      3. 验证 ETA 随时间减少
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-10-eta.txt

  Scenario: AdvancedSettings 折叠
    Tool: Playwright
    Steps:
      1. 打开 GUI
      2. AdvancedSettings 应默认折叠（用户看不到 LUT 等高级选项）
      3. 点击展开按钮 → 显示
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-10-fold.txt

  Scenario: 文件数提示
    Tool: Playwright
    Steps:
      1. 选含 5 个 EXR 的 input-dir
      2. 验证显示 "Found 5 EXR files"
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase4/task-10-count.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): ETA estimation + form UX polish (collapse advanced, file count, tooltips)`
  - Files: `gui/src/hooks/useProcessing.ts, gui/src/components/ProcessingView.tsx, gui/src/components/AdvancedSettings.tsx, gui/src/components/InputOutputPanel.tsx, gui/electron/main.ts, gui/electron/ipc-channels.ts`
  - Pre-commit: `cd gui; npm run build && npx playwright test`

- [ ] 11. Playwright e2e 覆盖新流程

  **What to do**:
  - **新建 `gui/tests/auto-detect.spec.ts`**:
    - 测 input-dir 自动检测 camera.json
    - 测 source resolution 解析
    - 测 fallback 场景（无 camera.json）
  - **新建 `gui/tests/resolution.spec.ts`**:
    - 测 ResolutionPanel preset 选择
    - 测 source 已知/未知两种模式
    - 测 scale 自动计算
  - **新建 `gui/tests/preview.spec.ts`**:
    - 测 frame_written → PNG 显示
    - 测 FFmpeg 缺失降级
  - **新建 `gui/tests/ndjson-parser.spec.ts`** (vitest 或 playwright):
    - 各 event 类型解析
    - 部分 JSON 行处理
  - **新建 `gui/tests/statusbar.spec.ts`**:
    - StatusBar 显示
    - exe 缺失降级
  - **新建 `gui/tests/new-controls.spec.ts`**:
    - source-fps / sharpness / ngx-log-level / tonemap-custom 控件
  - **更新 `gui/tests/real-processing.spec.ts`**:
    - 适配 NDJSON 输出格式
    - 验证 frame_written 事件流
  - **更新 `gui/tests/validation.spec.ts`**:
    - 加 custom tonemap 必须有 forward LUT 的验证
  - **fixtures**:
    - `gui/tests/fixtures/with_camera_json/` - 含 5 帧 EXR + camera.json
    - `gui/tests/fixtures/without_camera_json/` - 仅 5 帧 EXR

  **Must NOT do**:
  - 不删除现有测试（除非彻底失效）
  - 不假设 RTX 在测试机上（GPU-需要的测试用 SKIP）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: [`playwright`]

  **Parallelization**: NO (Wave E)
  **Blocked By**: Tasks 1-10

  **References**:
  - `gui/tests/*.spec.ts` 现有测试模式
  - `gui/tests/real-processing.spec.ts` 真实 e2e 模板

  **Acceptance Criteria**:
  - [ ] 6 个新 spec 文件全过
  - [ ] 现有 4 个 spec 不退化
  - [ ] 新 fixture 目录就绪

  **QA Scenarios**:
  ```
  Scenario: 全套 Playwright
    Tool: Bash
    Steps:
      1. cd gui; npx playwright test
    Expected Result: 100% pass (允许 GPU-required tests SKIP)
    Evidence: .sisyphus/evidence/phase4/task-11-playwright.txt
  ```

  **Commit**: YES
  - Message: `test(gui): playwright coverage for auto-detect / preview / resolution / NDJSON / statusbar`
  - Files: `gui/tests/auto-detect.spec.ts, gui/tests/resolution.spec.ts, gui/tests/preview.spec.ts, gui/tests/ndjson-parser.spec.ts, gui/tests/statusbar.spec.ts, gui/tests/new-controls.spec.ts, gui/tests/fixtures/*, gui/tests/real-processing.spec.ts (update), gui/tests/validation.spec.ts (update)`
  - Pre-commit: `cd gui; npx playwright test`

- [ ] 12. 文档更新

  **What to do**:
  - **`README.md`** 更新：
    - GUI 章节加截图（StatusBar / ResolutionPanel / Live preview）
    - 简化"快速开始"流程：
      1. 配置 exe path
      2. 选 input-dir（自动检测 camera.json + source resolution）
      3. 选 output-dir
      4. 选 target preset（如 4K）
      5. 点 Start
    - 移除过时的 GUI 描述（Phase 0 已删 C++ GUI）
  - **新建 `docs/gui_guide.md`**:
    - 详细 GUI 使用指南
    - 截图：每个面板、StatusBar、preview 区域
    - FAQ：camera.json 如何获得（指 Blender 导出）、为什么 source resolution 重要、preview 看不到怎么办
    - Troubleshooting: FFmpeg 缺失、DLSS unavailable
  - **`docs/usage_guide.md`** 更新：
    - 新增 CLI flag: `--source-fps`, `--sharpness`, `--ngx-log-level`, `--tonemap custom`, `--progress-format json`
    - 说明 camera.json auto-discovery 行为
    - 更新示例命令
  - **`docs/cli_reference.md`** (新建，如已有就更新):
    - 完整 CLI flag 列表 + 示例
    - 按用途分组（I/O / Quality / FG / Output / Diagnostics）
  - **`docs/architecture.md`** (可选):
    - GUI ↔ CLI 数据流图
    - NDJSON event 类型 schema
    - IPC channel 列表

  **Must NOT do**:
  - 不写过度营销文案
  - 不假设用户不熟悉命令行（提供两种入口）
  - 不删 C++ 文档章节（除非 Phase 0 已删）

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: []

  **Parallelization**: NO (Wave E)
  **Blocked By**: Task 11（文档要 reflect 真实工作的功能）

  **References**:
  - 现有 `README.md`, `docs/build_guide.md`, `docs/usage_guide.md`
  - 截图工具：可用 Playwright 自动生成（screenshot-evidence.spec.ts 模式）

  **Acceptance Criteria**:
  - [ ] README 含新流程
  - [ ] gui_guide.md 完整
  - [ ] CLI flag 全部文档化
  - [ ] 至少 5 张截图（StatusBar / ResolutionPanel / Live preview / Settings / Result）

  **QA Scenarios**:
  ```
  Scenario: 文档完整性
    Tool: Bash + Manual
    Steps:
      1. 阅读 README.md 快速开始章节
      2. 按步骤跑一遍（不查别处）
      3. 验证能完成一次处理
    Expected Result: 流程清晰、无歧义
    Evidence: .sisyphus/evidence/phase4/task-12-docs-walkthrough.txt
  ```

  **Commit**: YES
  - Message: `docs: update README + gui_guide.md + cli_reference for new GUI flow`
  - Files: `README.md, docs/gui_guide.md (new), docs/usage_guide.md, docs/cli_reference.md (new/update), docs/screenshots/*.png`
  - Pre-commit: 文档 lint（如配置）

---

## Final Verification Wave

> 4 个审查 agent 并行运行。全部通过后向用户展示并获明确 okay。

- [ ] F1. **计划合规审计** — `oracle`
  对每个 Must Have 验证。每个交付物的实际改动（grep / 文件存在性）确认。

  **QA Scenarios**:
  ```
  Scenario: Must Have 全验证
    Tool: Bash + Grep
    Steps:
      1. Select-String -Path "gui\src\components\BasicSettings.tsx" -Pattern "source-fps|sharpness" -SimpleMatch:$false  -- 应有匹配
      2. Select-String -Path "gui\src\components\AdvancedSettings.tsx" -Pattern "custom" -SimpleMatch:$false (in tonemap context)
      3. Select-String -Path "gui\electron\progress-parser.ts" -Pattern "ndjson|JSON\.parse" -SimpleMatch:$false  -- 新增 NDJSON 路径
      4. build\Release\dlss-compositor.exe --progress-format json --input-dir tests/fixtures/sequence/ --output-dir tmp/ --scale 2 2>&1 | findstr /c:"frame_written"  -- NDJSON 事件出现
      5. cd gui; npx playwright test tests/auto-detect.spec.ts  -- 通过
    Expected Result: 全部满足
    Evidence: .sisyphus/evidence/phase4/F1-must-have.txt
  ```

- [ ] F2. **代码质量审查** — `unspecified-high`
  Build C++ + 跑 ctest/pytest。GUI 端跑 `npm run build` + Playwright。无 TS lint 错误，无 React warning。

  **QA Scenarios**:
  ```
  Scenario: 全 stack 质量检查
    Tool: Bash
    Steps:
      1. cmake --build build --config Release 2>&1 | findstr /i "error warning"  -- 0 errors
      2. ctest --test-dir build --output-on-failure
      3. pytest tests/ -v
      4. cd gui; npm run build  -- 0 TS error
      5. cd gui; npm run lint (if configured)
    Expected Result: 全部 PASS
    Evidence: .sisyphus/evidence/phase4/F2-quality.txt
  ```

- [ ] F3. **真实 QA** — `unspecified-high`
  完整 e2e: 启动 Electron app → 选 input-dir（含 camera.json）→ 验证自动填充 + source res 检测 → 选 target=4K → scale 自动算 2.0 → 启动处理 → 看进度条 + live frame preview → 完成。

  **QA Scenarios**:
  ```
  Scenario: 完整无脑流程
    Tool: Playwright + Manual
    Steps:
      1. cd gui; npm run dev (or packaged)
      2. 选含 camera.json 的目录
      3. 验证：camera.json 自动填、source res 显示 1920x1080、target preset 默认 4K、scale 显示 2.0
      4. 选 output-dir
      5. 点 Start
      6. 观察：进度条更新、ETA 出现、frame preview 显示 PNG 缩略图
      7. 完成后 Status='done'
    Expected Result: 全程无报错，preview 真实显示
    Evidence: .sisyphus/evidence/phase4/F3-e2e.png + log
  ```

- [ ] F4. **范围保真度检查** — `deep`
  对每个 task 读 spec 和 diff，验证 1:1。无超范围改动。

  **QA Scenarios**:
  ```
  Scenario: 范围审计
    Tool: Git
    Steps:
      1. git log --oneline phase-4-start..HEAD  -- 列出本 phase 所有 commit
      2. git diff --stat phase-4-start HEAD  -- 影响文件
      3. 对照 Concrete Deliverables 列表
    Expected Result: 改动文件全部预期范围内
    Evidence: .sisyphus/evidence/phase4/F4-scope.txt
  ```

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `feat(cli): auto-discover camera.json from input directory` | src/cli/cli_parser.cpp, src/pipeline/sequence_processor.cpp, tests/test_cli.cpp | ctest |
| 2 | `feat(cli): add --progress-format json with NDJSON event stream` | src/cli/*, src/pipeline/sequence_processor.cpp, src/core/ndjson_emitter.*, tests/* | ctest |
| 3 | `feat(gui): expose --source-fps, --sharpness, --ngx-log-level controls` | gui/src/components/*, gui/electron/cli-args.ts, gui/src/types/dlss-config.ts | npm run build |
| 4 | `feat(gui): add 'custom' tonemap mode option` | gui/src/components/AdvancedSettings.tsx, gui/electron/cli-args.ts | npm run build |
| 5 | `feat(gui): use NDJSON progress events (replace regex parser)` | gui/electron/progress-parser.ts, gui/electron/main.ts, gui/electron/cli-args.ts | playwright |
| 6 | `feat(gui): NGX banner status bar (SDK version, DLSS-SR/FG availability)` | gui/src/components/StatusBar.tsx (new), gui/electron/main.ts, gui/electron/banner-parser.ts (new) | playwright |
| 7 | `feat(gui): auto-discover camera.json + source resolution detection` | gui/src/components/InputOutputPanel.tsx, gui/electron/main.ts, gui/electron/camera-detector.ts (new) | playwright |
| 8 | `feat(gui): target resolution + scale linkage (4K/1440p/1080p preset)` | gui/src/components/BasicSettings.tsx, gui/src/components/ResolutionPanel.tsx (new) | playwright |
| 9 | `feat(gui): live frame preview via FFmpeg PNG conversion` | gui/src/components/ProcessingView.tsx, gui/electron/main.ts, gui/electron/preview-generator.ts (new) | playwright |
| 10 | `feat(gui): ETA estimation + form UX polish` | gui/src/components/ProcessingView.tsx, gui/src/hooks/useProcessing.ts | playwright |
| 11 | `test(gui): playwright coverage for new auto-detect/preview/resolution flows` | gui/tests/* | playwright |
| 12 | `docs: update README + add gui_guide.md for new GUI flow` | README.md, docs/gui_guide.md | — |

---

## Success Criteria

### Verification Commands
```bash
# CLI 改动
ctest --test-dir build --output-on-failure
build\Release\dlss-compositor.exe --progress-format json --input-dir tests/fixtures/sequence/ --output-dir tmp/ --scale 2 | findstr /c:"frame_written"
build\Release\dlss-compositor.exe --input-dir tests/fixtures/with_camera_json/ --output-dir tmp/ --scale 2  # 应自动用 camera.json

# GUI build + tests
cd gui
npm run build
npx playwright test

# 完整 e2e（人 + 自动）
npm run dev  # 手动验证 5 项核心流程
```

### Final Checklist
- [ ] 12 个 commit 全过
- [ ] 用户用 GUI 完成一次 4K + 2x interpolation 处理，全程<10 次点击
- [ ] Live preview 真实显示 PNG
- [ ] NGX banner 顶部可见
- [ ] 所有 Phases 1-3 新增 CLI flag 在 GUI 可见
- [ ] CLI auto-discover camera.json 工作
- [ ] NDJSON output stable parseable
- [ ] 文档更新完毕
