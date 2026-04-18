# DLSS-SR 质量修复：Jitter、MV方向、Feature Flags

## TL;DR

> **目标**: 修复 DLSS-SR Preset L 效果不明显的问题。诊断已确认 3 个关键问题：jitter 始终为零（~70% 影响）、MV 方向可能错误（~20%）、缺少 AutoExposure flag。
>
> **交付物**:
> - EXR 通道名调查结果 + MV 方向验证数据
> - Blender 脚本添加 Halton jitter（camera shift + JSON sidecar）
> - Compositor 读取 jitter 数据
> - MV 方向修正（如验证确认需要）
> - AutoExposure flag 启用
> - 端到端渲染验证
>
> **预估工作量**: Medium
> **并行执行**: YES — 3 波次
> **关键路径**: Task 1 → Task 2/3 → Task 4 → Task 6 → Task 7 → Task 8 → F1-F4

---

## Context

### Original Request
用户发现 DLSS-SR Preset L（应为最强效果）处理后效果不明显。要求：(1) 审查整体管线正确性，(2) 对比 UE5 DLSS 插件确认 AOV 值是否符合 DLSS 输入要求。

### Interview Summary
**关键讨论**:
- 对比 UE5 DLSS 4.5 Plugin 全部源码，确认了 jitter=0 为最大问题
- MV 方向存在矛盾：代码注释声称 Blender=cur→prev（无需取反），但研究指出 Blender=prev→curr（需取反）
- 设计了 Blender camera shift_x/shift_y + Halton(2,3) jitter 方案，用户确认 UX 要求
- 用户有 Blender MCP 可直接渲染测试帧
- EXR validator 只看到 Image.RGBA 通道，但 exr_reader.cpp 有别名映射可能已处理

**研究发现**:
- UE5 `VelocityCombine.usf:148`: `-BackTemp * float2(0.5, -0.5)` — 取反+缩放
- UE5 `DLSSUpscaler.cpp`: 传递 `TemporalJitterPixels`（像素空间）
- DLSS SDK: "Jitter offset must be in input/render pixel space", [-0.5, +0.5] 范围
- Blender camera.data.shift_x/shift_y 以传感器尺寸为单位
- `camera_data_loader.cpp` 已存在，为 FG 管线读取 camera.json

### Metis Review
**识别的差距（已处理）**:
- MV 方向必须先经验验证，不能盲目取反
- Jitter 通过 camera shift 施加会被烘焙进 MV，可能导致双重校正
- 应扩展现有 camera.json 格式而非创建新 sidecar
- Blender 脚本有两份（aov_export_preset.py + __init__.py），需同步修改
- camera shift 单位转换：shift_x 是传感器宽度的分数，非像素
- render_cancel handler 必须恢复 camera 原始值

---

## Work Objectives

### Core Objective
通过实现 temporal jitter、修正 MV 方向（如验证需要）、启用 AutoExposure flag，使 DLSS-SR 能正确进行时域积累，充分发挥 Preset L 的上采样/降噪效果。

### Concrete Deliverables
- 修改后的 `blender/aov_export_preset.py` 和 `blender/dlss_compositor_aov/__init__.py`（含 jitter 功能）
- 扩展的 `camera.json` 格式（含 per-frame jitter_x/jitter_y）
- 修改后的 `src/core/camera_data_loader.cpp/.h`（读取 jitter 字段）
- 修改后的 `src/pipeline/sequence_processor.cpp`（将 jitter 传给 DlssSRFrameInput）
- 修改后的 `src/core/mv_converter.cpp`（如验证确认需取反）
- 修改后的 `src/dlss/ngx_wrapper.cpp`（AutoExposure flag）
- 对应的单元测试更新
- 端到端验证的渲染截图

### Definition of Done
- [ ] Blender 渲染 10 帧测试序列（带 jitter）→ camera.json 含非零 jitter 值
- [ ] Compositor 处理时日志显示非零 jitter offset
- [ ] DLSS-SR 输出在时域上稳定（无闪烁/游泳/鬼影）
- [ ] 所有 C++ 测试通过 (`ctest --test-dir build --output-on-failure`)
- [ ] 所有 Python 测试通过 (`pytest tests/ -v`)
- [ ] 用户视觉确认输出质量有显著提升

### Must Have
- Halton(2,3) jitter 序列，8 个采样点循环
- Jitter 值以像素空间存储于 camera.json
- Camera shift 在渲染完成/取消后恢复原始值
- camera.json jitter 字段可选（向后兼容）
- 每个修复独立可测试（独立 commit）

### Must NOT Have (Guardrails)
- ❌ 不修改 `dlss_fg_processor.cpp`（FG 管线单独处理）
- ❌ 不重写 `exr_validator.py`（仅调查通道名）
- ❌ 不添加 GBuffer hints 优化（albedo/normals/roughness 已有合理默认值）
- ❌ 不添加 GUI 控件（UX 优化不在此范围）
- ❌ 不修改 depth 处理（除非发现明确问题）
- ❌ 不添加 exposure texture 支持（AutoExposure flag 已足够）
- ❌ 不支持非 Blender EXR 来源
- ❌ 不在 MV 方向未经验验证前就盲目实施取反

---

## Verification Strategy

> **零人工干预** — 所有验证由 agent 执行。唯一例外：最终视觉质量由用户确认。

### Test Decision
- **基础设施存在**: YES (Catch2 for C++, pytest for Python)
- **自动化测试**: YES (Tests-after — 在实现后添加/更新测试)
- **框架**: Catch2 (C++), pytest (Python)

### QA Policy
每个 task 包含 agent 可执行的 QA 场景。
证据保存到 `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`。

- **EXR 分析**: 用 Bash (`python -c "..."`) 读取 EXR header
- **MV 验证**: 用 Blender MCP 渲染已知运动场景，读取像素值
- **C++ 测试**: 用 Bash (`ctest --test-dir build -R <test>`)
- **Python 测试**: 用 Bash (`pytest tests/ -v`)
- **管线集成**: 用 Bash 运行 compositor 并检查输出
- **视觉验证**: 用 Blender MCP 渲染测试帧后由用户确认

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 0 (诊断 — 必须先完成，门控后续实现):
├── Task 1: EXR 通道名调查 [quick]
├── Task 2: MV 方向验证（已知运动测试场景）[deep]
└── Task 3: Jitter-MV 交互验证 [deep]

Wave 1 (核心实现 — Wave 0 完成后):
├── Task 4: Blender Halton jitter 实现（两个脚本同步）[unspecified-high]
├── Task 5: AutoExposure flag 启用 [quick]
└── Task 6: MV 方向修复（条件性 — 依赖 Task 2 结果）[quick]

Wave 2 (集成 — Wave 1 完成后):
├── Task 7: Compositor 读取 jitter + 管线集成 [unspecified-high]
└── Task 8: 端到端验证渲染 + 视觉比较 [deep]

Wave FINAL (审查 — 全部 task 完成后):
├── F1: 计划合规审计 (oracle)
├── F2: 代码质量审查 (unspecified-high)
├── F3: 真实 QA (unspecified-high)
└── F4: 范围保真度检查 (deep)
→ 展示结果 → 获取用户确认
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 7 | 0 |
| 2 | — | 6 | 0 |
| 3 | — | 4 | 0 |
| 4 | 3 | 7, 8 | 1 |
| 5 | — | 8 | 1 |
| 6 | 2 | 8 | 1 |
| 7 | 1, 4 | 8 | 2 |
| 8 | 4, 5, 6, 7 | F1-F4 | 2 |

### Agent Dispatch Summary

- **Wave 0**: 3 tasks — T1 → `quick`, T2 → `deep`, T3 → `deep`
- **Wave 1**: 3 tasks — T4 → `unspecified-high`, T5 → `quick`, T6 → `quick`
- **Wave 2**: 2 tasks — T7 → `unspecified-high`, T8 → `deep`
- **FINAL**: 4 tasks — F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [x] 1. EXR 通道名调查

  **What to do**:
  - 用 Python OpenEXR 直接读取 AOV EXR 文件的 header，列出所有通道名
  - 目标文件: `E:\Render Output\Compositing\SnowMix\sequences\4k_aov\file_name_0200.exr`（或附近帧）
  - 验证 exr_reader.cpp 的别名映射 (`Image.`→`RenderLayer.Combined.`, `Vector.`→`RenderLayer.Vector.` 等) 是否能正确匹配实际通道名
  - 验证 channel_mapper.cpp 是否能找到必需通道（color RGBA, depth, mvX/mvY）
  - 如果通道名不匹配，记录实际通道名和需要的映射修改
  - 特别检查是否为 multi-part EXR（OpenEXR 3.x 支持）

  **Must NOT do**:
  - 不重写 exr_validator.py
  - 不修改任何源代码（仅调查）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 0 (with Tasks 2, 3)
  - **Blocks**: Task 7 (compositor 集成需要知道通道映射是否正确)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/core/exr_reader.cpp` — EXR 读取逻辑，包含通道名别名映射表（`applyAliases` 函数）
  - `src/core/channel_mapper.cpp` — 通道到 DLSS buffer 的映射逻辑，特别看 `mapChannels` 函数的 fallback 行为

  **API/Type References**:
  - `src/core/exr_reader.h` — ExrReader 接口，包括 `channelNames()` 方法

  **External References**:
  - OpenEXR Python API: `OpenEXR.InputFile(path).header()['channels']`

  **WHY Each Reference Matters**:
  - `exr_reader.cpp` 的别名映射决定了 compositor 能否找到正确通道 — 如果实际通道名不在别名表中，DLSS 将收到零/默认值
  - `channel_mapper.cpp` 的 fallback 行为决定了缺失通道时是报错还是静默使用默认值

  **Acceptance Criteria**:

  **QA Scenarios:**

  ```
  Scenario: 列出 EXR 所有通道名
    Tool: Bash (python)
    Preconditions: Python 3 + OpenEXR 库已安装
    Steps:
      1. python -c "import OpenEXR; f=OpenEXR.InputFile(r'E:\Render Output\Compositing\SnowMix\sequences\4k_aov\file_name_0200.exr'); channels=sorted(f.header()['channels'].keys()); print('Channel count:', len(channels)); [print(c) for c in channels]"
      2. 验证输出包含以下通道（或其 alias）：
         - Combined/Image: R, G, B, A
         - Depth: Z
         - Vector: X, Y (至少)
         - Normal: X, Y, Z
    Expected Result: 通道列表已获取并记录，包含所有 DLSS 必需的通道类型
    Failure Indicators: 只有 Image.A/B/G/R 四个通道且无其他通道 → Blender 导出配置有问题
    Evidence: .sisyphus/evidence/task-1-exr-channels.txt

  Scenario: 验证 exr_reader 别名映射覆盖
    Tool: Bash (grep)
    Preconditions: Task 1 scenario 1 完成
    Steps:
      1. 对比 scenario 1 获取的实际通道名与 exr_reader.cpp 中 applyAliases 的映射表
      2. 确认每个实际通道名都能被别名表匹配
    Expected Result: 所有必需通道的实际名称都在别名映射覆盖范围内
    Failure Indicators: 某些通道名不在别名表中
    Evidence: .sisyphus/evidence/task-1-alias-coverage.txt
  ```

  **Commit**: YES (group with Tasks 2, 3)
  - Message: `investigate(sr): verify EXR channel names and MV direction`
  - Files: 调查结果文件
  - Pre-commit: —

- [x] 2. MV 方向验证（已知运动测试场景）

  **What to do**:
  - 在用户的 Blender 项目中创建一个简单测试：一个物体在 X 轴正方向移动（如从 x=0 到 x=1）
  - 渲染 2 帧（frame 200, 201），输出到临时目录
  - 用 Python 读取 Vector.X/Y 通道的像素值（在物体区域）
  - 确定值的符号：
    - 如果 Vector.X > 0 → Blender 表示 curr−prev（物体移动方向）
    - 如果 Vector.X < 0 → Blender 表示 prev−curr（反方向）
  - 与 DLSS 要求的 curr→prev 方向对比，确定是否需要取反
  - **关键**：这个测试决定 Task 6 是否需要执行

  **Must NOT do**:
  - 不修改 mv_converter.cpp（仅验证）
  - 不污染用户现有渲染数据

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []
    - 需要使用 Blender MCP 创建测试场景和渲染

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 0 (with Tasks 1, 3)
  - **Blocks**: Task 6 (MV 方向修复取决于此验证结果)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `src/core/mv_converter.cpp:3-15` — 当前代码注释声称 Blender X = cur−prev，DLSS 同意，无需取反
  - `tests/test_mv_converter.cpp:8-23` — 现有测试用例：10px right motion 期望输出 10.0（正值），如果需要取反这个测试需要更新

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_helpers_vk.h` — `InMVScaleX/Y` 说明：乘以存储值得到像素位移
  - `DLSS/include/nvsdk_ngx_defs.h` — DLSS MV 约定文档字符串

  **External References**:
  - UE5 `VelocityCombine.usf:148` — `-BackTemp * float2(0.5, -0.5)` — UE5 做了取反
  - Blender docs: Vector pass = "speed of the pixels from one frame to the next"

  **WHY Each Reference Matters**:
  - `mv_converter.cpp` 注释是当前实现的根据 — 如果注释错误，这是 ~20% 质量问题的来源
  - UE5 VelocityCombine 做了取反 — 但 UE5 的 MV 约定可能与 Blender 不同，不能直接照搬
  - 只有实际像素数据能给出确定答案

  **Acceptance Criteria**:

  **QA Scenarios:**

  ```
  Scenario: 渲染已知运动测试帧
    Tool: Blender MCP (execute_blender_code)
    Preconditions: Blender 项目已打开且 MCP 已连接
    Steps:
      1. 在 Blender 中创建一个 cube，frame 200 位于 x=0，frame 201 位于 x=2（明确向右运动）
      2. 确保 Vector pass 已启用
      3. 设置临时输出路径（如 //tmp_mv_test/）
      4. 渲染 frame 200 和 201 为 MultiLayer EXR
      5. 用 Python 读取 frame 201 的 Vector.X 在 cube 中心区域的像素值
      6. 记录值的符号和大小
    Expected Result: Vector.X 值明确为正或负，与理论预测一致
    Failure Indicators: Vector.X ≈ 0 → 测试场景运动不足或通道读取错误
    Evidence: .sisyphus/evidence/task-2-mv-direction.txt

  Scenario: 静态区域 MV 为零
    Tool: Bash (python)
    Preconditions: 上一场景渲染完成
    Steps:
      1. 读取同一帧的背景区域（无运动）Vector.X/Y
      2. 验证值接近 0.0
    Expected Result: 静态区域 |Vector.X| < 0.01 且 |Vector.Y| < 0.01
    Failure Indicators: 静态区域有大幅 MV 值 → 通道读取可能有问题
    Evidence: .sisyphus/evidence/task-2-mv-static.txt
  ```

  **Commit**: YES (group with Task 1)
  - Message: `investigate(sr): verify EXR channel names and MV direction`
  - Files: 验证结果
  - Pre-commit: —

- [x] 3. Jitter-MV 交互验证

  **What to do**:
  - 验证当 camera.data.shift_x/shift_y 在帧间变化时，Blender 的 Vector pass 是否会包含 jitter 引起的位移
  - 测试方法：
    1. 渲染一个完全静态的场景（无物体运动、无相机运动）2 帧 → Vector 应全为 0
    2. 第二次渲染同一场景，但 frame 201 施加 shift_x += 0.001 → 检查 Vector 值
    3. 如果 Vector 值变为非零 → jitter 被烘焙进 MV → 需要在 compositor 端做 jitter 补偿
    4. 如果 Vector 值仍为 0 → jitter 不影响 MV → DLSS 需要自行处理 jitter
  - 这个结果决定 Task 4（Blender jitter 实现）是否需要同时输出"补偿用的 jitter delta"给 compositor
  - 同时验证 shift_x 的换算关系：1 个 shift_x 单位 = 多少像素

  **Must NOT do**:
  - 不修改任何源代码
  - 不污染用户现有数据

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 0 (with Tasks 1, 2)
  - **Blocks**: Task 4 (jitter 实现方案取决于此结果)
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `blender/aov_export_preset.py:52-55` — 当前相机属性读取方式（fov, clip_start, clip_end）
  - `src/core/camera_data_loader.cpp` — camera.json 的解析逻辑和字段结构

  **API/Type References**:
  - Blender API: `camera.data.shift_x`, `camera.data.shift_y` — 以传感器宽度为单位的偏移

  **External References**:
  - Blender docs: Camera Shift — "Shift the camera horizontally or vertically. Allows for the correction of perspective distortion."
  - DLSS SDK: MVJittered flag — 如果 MV 包含 jitter，设此 flag；如果不包含则不设

  **WHY Each Reference Matters**:
  - Blender shift 的行为直接决定 DLSS 的 MVJittered flag 是否需要设置
  - 如果 jitter 被烘焙进 MV，可能需要更复杂的处理（从 MV 中减去 jitter 位移，或设置 MVJittered flag）

  **Acceptance Criteria**:

  **QA Scenarios:**

  ```
  Scenario: 静态场景 + 无 shift → 零 MV 基线
    Tool: Blender MCP
    Preconditions: Blender 已打开，静态场景
    Steps:
      1. 确保场景完全静态（无动画、无相机运动）
      2. camera.data.shift_x = 0, shift_y = 0
      3. 渲染 frame 200, 201 到临时目录
      4. 读取 frame 201 Vector.X/Y，确认全为 0
    Expected Result: 所有像素 |Vector.X| < 0.001 且 |Vector.Y| < 0.001
    Evidence: .sisyphus/evidence/task-3-baseline-zero-mv.txt

  Scenario: 静态场景 + 帧间 shift 变化 → 检查 MV 是否包含 jitter
    Tool: Blender MCP
    Preconditions: 基线测试完成
    Steps:
      1. 用 frame_change_pre handler 在 frame 201 设置 shift_x += 0.001
      2. 渲染 frame 200, 201
      3. 读取 frame 201 Vector.X/Y
      4. 如果 Vector 值非零 → jitter 被烘焙进 MV
      5. 记录 shift_x=0.001 对应的像素位移量（用于换算公式验证）
    Expected Result: 明确结论：jitter 是否烘焙进 MV + 换算系数
    Failure Indicators: Vector 值接近零但不完全为零 → 可能是浮点精度问题
    Evidence: .sisyphus/evidence/task-3-jitter-mv-interaction.txt

  Scenario: 换算关系验证
    Tool: Bash (python)
    Preconditions: 前两个场景完成
    Steps:
      1. 计算 shift_x=0.001 对应的理论像素位移: 0.001 * resolution_x
      2. 对比实际 MV 值（如果非零）
      3. 确认换算公式: jitter_pixels = shift_x * resolution_x
    Expected Result: 理论值与实际值匹配（误差 < 5%），或确认 MV 不包含 jitter
    Evidence: .sisyphus/evidence/task-3-conversion-formula.txt
  ```

  **Commit**: YES (group with Tasks 1, 2)
  - Message: `investigate(sr): verify EXR channel names and MV direction`
  - Files: 验证结果
  - Pre-commit: —

- [x] 4. Blender Halton Jitter 实现

  **What to do**:
  - 在 `blender/aov_export_preset.py` 和 `blender/dlss_compositor_aov/__init__.py` 中实现 jitter 功能
  - 实现内容：
    1. **Halton 序列生成器**: `halton(index, base)` 函数，支持 base 2 和 3
    2. **Render handler 注册**: 在 `configure_passes` 操作中（或新的 `configure_dlss_jitter` 操作中）注册：
       - `render_init` handler: 保存原始 camera.data.shift_x/shift_y
       - `frame_change_pre` handler: 计算并施加 jitter
         ```python
         jitter_x = halton(frame_num % 8, 2) - 0.5  # [-0.5, +0.5) pixels
         jitter_y = halton(frame_num % 8, 3) - 0.5
         # 像素空间 → Blender shift 空间
         camera.data.shift_x = original_shift_x + jitter_x / render_resolution_x
         camera.data.shift_y = original_shift_y + jitter_y / render_resolution_y
         ```
       - `render_complete` + `render_cancel` handler: 恢复原始 shift 值
    3. **Jitter 数据写入 camera.json**: 在 `export_camera` operator 中，为每帧添加 `jitter_x` 和 `jitter_y` 字段（像素空间值，不是 shift 单位）
    4. **注意**: 如果 Task 3 发现 jitter 被烘焙进 MV，需要设置 MVJittered flag（在 Task 7 中处理）；如果 jitter 不影响 MV，则保持当前逻辑
  - **两个脚本必须同步修改**：`aov_export_preset.py`（独立脚本版）和 `__init__.py`（addon 版）

  **Must NOT do**:
  - 不添加 GUI 控件（jitter 应自动工作，无需用户调参）
  - 不修改 C++ 代码（那是 Task 7）
  - 不使用超过 8 个 Halton 采样点（DLSS 标准）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 5, 6)
  - **Blocks**: Task 7, Task 8
  - **Blocked By**: Task 3 (需要知道 jitter-MV 交互结果)

  **References**:

  **Pattern References**:
  - `blender/aov_export_preset.py:27-105` — `export_camera` operator 的完整实现，展示了如何遍历帧、读取相机数据、写入 JSON
  - `blender/aov_export_preset.py:108-300` — `configure_passes` operator，展示了如何配置渲染设置和节点树
  - `blender/aov_export_preset.py:385-398` — `register()/unregister()` 函数，新 handler 需要在这里注册/注销

  **API/Type References**:
  - Blender API: `bpy.app.handlers.frame_change_pre`, `bpy.app.handlers.render_init`, `bpy.app.handlers.render_complete`, `bpy.app.handlers.render_cancel`
  - Blender API: `camera.data.shift_x`, `camera.data.shift_y` — float，以传感器尺寸为单位
  - `src/core/camera_data_loader.h:6-17` — `CameraFrameData` struct，新字段应添加到这里对应的 JSON 格式中

  **External References**:
  - DLSS SDK: Halton(2,3) 是标准 jitter 序列，8 个采样点循环
  - UE5 `DLSSUpscaler.cpp` — 传递 `TemporalJitterPixels`（像素空间）

  **WHY Each Reference Matters**:
  - `export_camera` 已有完整的帧遍历+JSON写入模板 — jitter 数据应直接添加到同一结构
  - `register()` 中需要添加 handler 的 append/remove — 遵循现有注册模式
  - `CameraFrameData` struct 将在 Task 7 中扩展，JSON 格式必须匹配

  **Acceptance Criteria**:
  - [ ] `aov_export_preset.py` 和 `__init__.py` 都已更新
  - [ ] Halton 序列生成 8 个不重复的值，范围 [0, 1)
  - [ ] Jitter 值以像素空间写入 camera.json
  - [ ] camera shift 在渲染结束/取消后恢复

  **QA Scenarios:**

  ```
  Scenario: Halton 序列正确性
    Tool: Bash (python)
    Preconditions: 修改后的脚本可被导入
    Steps:
      1. 导入 halton 函数
      2. 生成 halton(0..7, 2) 和 halton(0..7, 3) 的值
      3. 验证: 所有 8 个值不重复，范围 [0, 1)
      4. 验证: halton(0, 2)=0.0, halton(1, 2)=0.5, halton(2, 2)=0.25, halton(3, 2)=0.75
    Expected Result: 8 个值与标准 Halton 序列完全匹配
    Evidence: .sisyphus/evidence/task-4-halton-sequence.txt

  Scenario: Camera shift 恢复
    Tool: Blender MCP
    Preconditions: Jitter handlers 已注册
    Steps:
      1. 记录原始 camera.data.shift_x/shift_y 值
      2. 触发一次渲染（1-2 帧即可）
      3. 渲染完成后检查 camera.data.shift_x/shift_y
    Expected Result: shift 值恢复到原始值（误差 < 1e-8）
    Failure Indicators: shift 值改变 → handler 注册/清理逻辑有 bug
    Evidence: .sisyphus/evidence/task-4-shift-restore.txt

  Scenario: camera.json 包含 jitter 数据
    Tool: Bash (python)
    Preconditions: 带 jitter 的渲染完成
    Steps:
      1. 读取生成的 camera.json
      2. 验证每帧数据中包含 "jitter_x" 和 "jitter_y" 字段
      3. 验证至少部分帧的 jitter 值非零
      4. 验证 jitter 值范围在 [-0.5, +0.5]
    Expected Result: camera.json 中所有帧都有 jitter_x/jitter_y，值在合理范围内
    Failure Indicators: 字段缺失或值全为 0
    Evidence: .sisyphus/evidence/task-4-camera-json-jitter.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): add Halton jitter to AOV export scripts`
  - Files: `blender/aov_export_preset.py`, `blender/dlss_compositor_aov/__init__.py`
  - Pre-commit: `pytest tests/ -v`

- [x] 5. 启用 AutoExposure Flag

  **What to do**:
  - 在 `src/dlss/ngx_wrapper.cpp` 中为 DLSS-SR feature creation 添加 `AutoExposure` flag
  - 当前代码（行 174-175）:
    ```cpp
    createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    ```
  - 修改为:
    ```cpp
    createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    ```
  - 原因: 当前管线没有提供 exposure texture（`InPreExposure=1.0f`, `InExposureScale=1.0f`），DLSS SDK 建议在此情况下启用 AutoExposure

  **Must NOT do**:
  - 不添加 exposure texture 支持
  - 不修改 `InPreExposure` 或 `InExposureScale` 值
  - 不修改 FG 管线的 flags

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 4, 6)
  - **Blocks**: Task 8
  - **Blocked By**: None (可独立执行)

  **References**:

  **Pattern References**:
  - `src/dlss/ngx_wrapper.cpp:168-175` — 当前 DLSS-SR feature creation 代码
  - `src/dlss/dlss_sr_processor.cpp:159-160` — `InPreExposure=1.0f`, `InExposureScale=1.0f`

  **API/Type References**:
  - `DLSS/include/nvsdk_ngx_defs.h` — `NVSDK_NGX_DLSS_Feature_Flags_AutoExposure` 枚举值定义

  **External References**:
  - UE5 `NGXRHI.cpp` — UE5 默认启用 AutoExposure

  **WHY Each Reference Matters**:
  - `ngx_wrapper.cpp:174-175` 是唯一需要修改的位置 — 添加一个 flag 即可
  - SDK header 确认 flag 名称和语义

  **Acceptance Criteria**:
  - [ ] `ngx_wrapper.cpp` feature flags 包含 AutoExposure
  - [ ] C++ 构建成功

  **QA Scenarios:**

  ```
  Scenario: AutoExposure flag 已设置
    Tool: Bash (grep)
    Preconditions: 代码已修改
    Steps:
      1. grep "AutoExposure" src/dlss/ngx_wrapper.cpp
      2. 确认 NVSDK_NGX_DLSS_Feature_Flags_AutoExposure 在 InFeatureCreateFlags 中
    Expected Result: grep 命中 1 行，包含 AutoExposure flag
    Failure Indicators: grep 无结果
    Evidence: .sisyphus/evidence/task-5-autoexposure-flag.txt

  Scenario: 构建成功
    Tool: Bash (cmake)
    Preconditions: 修改已保存
    Steps:
      1. cmake --build build --config Release
      2. 验证编译无错误
    Expected Result: Build succeeded
    Failure Indicators: 编译错误
    Evidence: .sisyphus/evidence/task-5-build.txt
  ```

  **Commit**: YES
  - Message: `fix(ngx): enable AutoExposure flag for DLSS-SR`
  - Files: `src/dlss/ngx_wrapper.cpp`
  - Pre-commit: `ctest --test-dir build --output-on-failure`

- [x] 6. MV 方向修复（条件性 — 依赖 Task 2 结果）

  **What to do**:
  - **前提**: 仅当 Task 2 验证结果确认 Blender MV 方向与 DLSS 期望不一致时执行
  - 如果需要取反，修改 `src/core/mv_converter.cpp`:
    ```cpp
    // 修改前
    result.mvXY[i * 2 + 0] = blenderMv4[i * 4 + 0];
    result.mvXY[i * 2 + 1] = blenderMv4[i * 4 + 1];
    
    // 修改后
    result.mvXY[i * 2 + 0] = -blenderMv4[i * 4 + 0];
    result.mvXY[i * 2 + 1] = -blenderMv4[i * 4 + 1];
    ```
  - 更新代码注释以反映真实约定
  - 更新 `tests/test_mv_converter.cpp` 中的测试用例：
    - "10px right motion" 测试期望值从 10.0 改为 -10.0
    - "Y axis" 测试期望值从 5.0 改为 -5.0
    - "channels 2,3 ignored" 测试期望值从 (3.0, 4.0) 改为 (-3.0, -4.0)
  - **如果 Task 2 确认不需要取反**: 标记此 Task 为 SKIP，更新代码注释以记录验证结果

  **Must NOT do**:
  - 不在 Task 2 验证完成前执行修改
  - 不修改 scale 值（保持 1.0）
  - 不修改 channels 2, 3 的处理

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 4, 5)
  - **Blocks**: Task 8
  - **Blocked By**: Task 2 (必须等待 MV 方向验证结果)

  **References**:

  **Pattern References**:
  - `src/core/mv_converter.cpp:17-33` — 完整的 `convert()` 函数，修改点在行 23-24
  - `tests/test_mv_converter.cpp:8-78` — 所有现有测试用例，需要更新期望值

  **API/Type References**:
  - `src/core/mv_converter.h` — `MvConvertResult` struct 定义（mvXY vector + scaleX/scaleY）

  **External References**:
  - Task 2 的验证结果 — `.sisyphus/evidence/task-2-mv-direction.txt`

  **WHY Each Reference Matters**:
  - `mv_converter.cpp:23-24` 是唯一的修改点 — 添加负号
  - 测试文件必须同步更新，否则 CI 会失败

  **Acceptance Criteria**:
  - [ ] 如果需要取反: mv_converter.cpp 已取反，测试已更新且通过
  - [ ] 如果不需要取反: Task 标记为 SKIP，注释已更新
  - [ ] `ctest --test-dir build -R mv --output-on-failure` 通过

  **QA Scenarios:**

  ```
  Scenario: MV 取反后测试通过
    Tool: Bash (ctest)
    Preconditions: mv_converter.cpp 和 test_mv_converter.cpp 已修改
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build -R mv --output-on-failure
    Expected Result: 所有 mv 相关测试通过
    Failure Indicators: 测试失败 → 修改不一致
    Evidence: .sisyphus/evidence/task-6-mv-tests.txt

  Scenario: 10px right motion 输出为 -10
    Tool: Bash (ctest)
    Preconditions: 代码已修改
    Steps:
      1. 运行 test "MvConverter - 10px right motion converts to (-10, 0)"
      2. 验证输出 mvXY[0] == -10.0f, mvXY[1] == 0.0f
    Expected Result: 测试通过
    Evidence: .sisyphus/evidence/task-6-mv-negation-verify.txt
  ```

  **Commit**: YES
  - Message: `fix(mv): negate MV X,Y for DLSS curr→prev convention`
  - Files: `src/core/mv_converter.cpp`, `tests/test_mv_converter.cpp`
  - Pre-commit: `ctest --test-dir build -R mv --output-on-failure`

- [x] 7. Compositor 读取 Jitter + 管线集成

  **What to do**:
  - **扩展 CameraFrameData struct** (`src/core/camera_data_loader.h`):
    ```cpp
    struct CameraFrameData {
        // ... 现有字段 ...
        float jitter_x = 0.0f;  // 像素空间，[-0.5, +0.5]
        float jitter_y = 0.0f;  // 像素空间，[-0.5, +0.5]
    };
    ```
  - **扩展 JSON 解析** (`src/core/camera_data_loader.cpp`):
    - 在帧数据解析中，可选读取 `jitter_x` 和 `jitter_y`
    - 字段不存在时默认为 0.0（向后兼容旧 camera.json）
  - **SR 管线集成** (`src/pipeline/sequence_processor.cpp`):
    - 在 SR-only 流程中加载 camera.json（目前仅 FG/SRFG 流程加载）
    - 将 jitter 值传递给 `DlssSRFrameInput`:
      ```cpp
      frame.jitterX = cameraLoader.hasFrame(frameNum) ? 
                       cameraLoader.getFrame(frameNum).jitter_x : 0.0f;
      frame.jitterY = cameraLoader.hasFrame(frameNum) ?
                       cameraLoader.getFrame(frameNum).jitter_y : 0.0f;
      ```
    - 添加 verbose 日志输出 jitter 值
  - **处理 MVJittered flag** (如果 Task 3 确认 jitter 被烘焙进 MV):
    - 在 `ngx_wrapper.cpp` 的 feature create flags 中添加 `NVSDK_NGX_DLSS_Feature_Flags_MVJittered`
  - **更新测试**:
    - `test_camera_data_loader.cpp`: 添加 jitter 字段解析测试（含有/不含 jitter 两种情况）
    - 验证向后兼容：旧 camera.json（无 jitter 字段）仍能正确加载

  **Must NOT do**:
  - 不修改 FG 管线的 jitter 处理
  - 不创建新的 sidecar 文件格式（使用现有 camera.json）
  - 不硬编码 Halton 值（compositor 只读取 JSON 中的值）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (sequential dependency)
  - **Blocks**: Task 8
  - **Blocked By**: Task 1 (通道映射确认), Task 4 (Blender jitter 实现)

  **References**:

  **Pattern References**:
  - `src/core/camera_data_loader.h:6-17` — `CameraFrameData` struct，新字段添加位置
  - `src/core/camera_data_loader.cpp` — JSON 解析逻辑，参考现有字段的解析方式（`fov`, `aspect_ratio` 等）
  - `src/pipeline/sequence_processor.cpp:911-913` — FG 管线中 camera data 加载的模式（SR 管线需要类似添加）
  - `src/pipeline/sequence_processor.cpp:750-767` — SR 管线中 frame 参数赋值位置（jitter 应在此添加）

  **API/Type References**:
  - `src/dlss/dlss_sr_processor.h:34-35` — `DlssSRFrameInput` 中的 `jitterX`, `jitterY` 字段
  - `DLSS/include/nvsdk_ngx_defs.h` — `NVSDK_NGX_DLSS_Feature_Flags_MVJittered`

  **Test References**:
  - `tests/test_camera_data_loader.cpp:13-55` — 现有 camera JSON 测试数据和测试模式
  - `tests/test_camera_data_loader.cpp:129-191` — 错误处理测试模式

  **WHY Each Reference Matters**:
  - `camera_data_loader.cpp` 的 JSON 解析模式必须遵循 — 使用 nlohmann::json，字段通过 `.value()` 或 `.at()` 获取
  - `sequence_processor.cpp:911-913` 展示了 FG 管线如何加载 camera data — SR 管线需要复制类似逻辑但可选（camera data 对 SR 是可选的）
  - 行 767 附近是赋值 jitter 的唯一正确位置 — 在 `frame.reset` 赋值之前

  **Acceptance Criteria**:
  - [ ] `CameraFrameData` 包含 `jitter_x`, `jitter_y` 字段
  - [ ] 旧 camera.json（无 jitter 字段）仍能加载（jitter 默认 0.0）
  - [ ] 新 camera.json（含 jitter 字段）正确解析
  - [ ] SR 管线将 jitter 传递给 DLSS evaluate
  - [ ] verbose 模式输出非零 jitter 值
  - [ ] C++ 测试通过

  **QA Scenarios:**

  ```
  Scenario: 向后兼容 — 旧 camera.json 不含 jitter
    Tool: Bash (ctest)
    Preconditions: 代码已修改并构建
    Steps:
      1. 运行现有 camera_data_loader 测试（使用旧格式 JSON）
      2. 验证所有测试通过
      3. 新增测试验证 jitter 默认为 0.0
    Expected Result: 所有测试通过，jitter 默认为 0.0
    Evidence: .sisyphus/evidence/task-7-backward-compat.txt

  Scenario: 新 camera.json 含 jitter 字段
    Tool: Bash (ctest)
    Preconditions: 新测试已编写
    Steps:
      1. 创建包含 jitter_x/jitter_y 字段的测试 JSON
      2. 加载并验证 jitter 值正确解析
      3. 验证 jitter_x=0.25, jitter_y=-0.125 时读取值匹配
    Expected Result: jitter 值正确解析
    Evidence: .sisyphus/evidence/task-7-jitter-parsing.txt

  Scenario: SR 管线集成 — verbose 输出 jitter
    Tool: Bash
    Preconditions: compositor 已构建，有带 jitter 的 camera.json
    Steps:
      1. 运行 compositor: dlss-compositor.exe --input-dir <test_frames> --output-dir <tmp> --scale 2 --camera-data <jittered_camera.json> --verbose
      2. 检查输出中包含非零 jitter 信息
    Expected Result: verbose 输出显示 "Jitter: X=0.xxxx Y=0.yyyy"（非零值）
    Failure Indicators: jitter 显示为 0.0 或无 jitter 输出行
    Evidence: .sisyphus/evidence/task-7-sr-jitter-integration.txt
  ```

  **Commit**: YES
  - Message: `feat(compositor): read jitter from camera.json and wire to DLSS-SR`
  - Files: `src/core/camera_data_loader.h`, `src/core/camera_data_loader.cpp`, `src/pipeline/sequence_processor.cpp`, `tests/test_camera_data_loader.cpp`
  - Pre-commit: `ctest --test-dir build --output-on-failure`

- [x] 8. 端到端验证渲染 + 视觉比较

  **What to do**:
  - 使用 Blender MCP 渲染一个测试序列（10 帧，从 frame 200 开始），启用 jitter
  - 降低分辨率以加速（如 960x540 → 1920x1080 with scale 2）
  - 运行修复后的 compositor 处理整个序列
  - 与之前的输出进行视觉比较：
    1. 对比修复前/后的同一帧 — 是否更清晰、降噪更好
    2. 检查时域稳定性 — 连续帧之间是否有闪烁/鬼影
    3. 检查边缘锐度 — 上采样效果是否明显
  - 将对比截图保存为证据
  - **通知用户进行最终视觉确认**

  **Must NOT do**:
  - 不修改任何源代码
  - 不使用用户现有的 4k_aov 序列（那些没有 jitter）

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (after all other tasks)
  - **Blocks**: F1-F4
  - **Blocked By**: Tasks 4, 5, 6, 7

  **References**:

  **Pattern References**:
  - `blender/aov_export_preset.py` — 完整的 pass 配置和 camera export 流程
  - CLI README 中的使用示例

  **External References**:
  - `E:\Render Output\Compositing\SnowMix\sequences\4k_dlss\` — 现有（无 jitter）DLSS 输出，作为对比基线

  **WHY Each Reference Matters**:
  - 现有 4k_dlss 输出是修复前的基线 — 与修复后输出对比可视化差异

  **Acceptance Criteria**:
  - [ ] 10 帧测试序列渲染完成（带 jitter）
  - [ ] camera.json 包含非零 jitter 值
  - [ ] Compositor 处理完成且无错误
  - [ ] 输出帧文件存在且非空
  - [ ] 截图对比保存为证据

  **QA Scenarios:**

  ```
  Scenario: 渲染带 jitter 的测试序列
    Tool: Blender MCP
    Preconditions: Blender 项目已打开，jitter handlers 已注册
    Steps:
      1. 设置渲染分辨率为 960x540（或适合快速测试的分辨率）
      2. 设置帧范围为 200-209
      3. 设置输出路径为临时目录
      4. 执行 Configure Passes + Export Camera Data
      5. 渲染动画
      6. 验证输出目录包含 10 个 EXR 文件和 camera.json
    Expected Result: 10 个 EXR 文件 + camera.json（含 jitter 字段）
    Evidence: .sisyphus/evidence/task-8-render-output.txt

  Scenario: Compositor 处理带 jitter 的序列
    Tool: Bash
    Preconditions: 渲染完成
    Steps:
      1. dlss-compositor.exe --input-dir <rendered_frames> --output-dir <output> --scale 2 --preset L --camera-data <camera.json> --verbose
      2. 验证处理完成且无错误
      3. 验证输出目录包含 10 个输出帧
      4. 检查 verbose 日志中 jitter 值非零
    Expected Result: 10 个处理后的输出帧，jitter 已生效
    Failure Indicators: 错误退出码、输出帧缺失、jitter 为零
    Evidence: .sisyphus/evidence/task-8-compositor-output.txt

  Scenario: 视觉质量对比（需用户确认）
    Tool: Bash (截图/查看)
    Preconditions: 修复前和修复后输出都已就绪
    Steps:
      1. 选取代表性帧（如 frame 205）
      2. 并排展示修复前 vs 修复后输出
      3. 检查：边缘锐度、降噪效果、鬼影/闪烁
      4. 通知用户进行视觉确认
    Expected Result: 用户确认输出质量有可感知提升
    Failure Indicators: 用户报告无差异或质量下降
    Evidence: .sisyphus/evidence/task-8-visual-comparison.png
  ```

  **Commit**: YES
  - Message: `test(e2e): end-to-end verification with all DLSS-SR fixes`
  - Files: 证据文件
  - Pre-commit: —

---

## Final Verification Wave

> 4 个审查 agent 并行运行。全部必须通过。向用户展示结果并获得明确确认后才能完成。

- [x] F1. **计划合规审计** — `oracle`
  读取完整计划。对每个 "Must Have"：验证实现存在（读文件、运行命令）。对每个 "Must NOT Have"：搜索代码库中禁止的模式 — 找到则拒绝并附 file:line。检查 `.sisyphus/evidence/` 中的证据文件。对比交付物与计划。
  输出: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [x] F2. **代码质量审查** — `unspecified-high`
  运行 build + 测试。审查所有修改文件：`as any`/`@ts-ignore`、空 catch、console.log、注释掉的代码、未使用的 import。检查 AI slop：过多注释、过度抽象、通用命名。
  输出: `Build [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [x] F3. **真实 QA** — `unspecified-high`
  从干净状态开始。执行每个 task 的每个 QA 场景。测试跨 task 集成。测试边缘情况：空状态、无效输入、快速动作。保存到 `.sisyphus/evidence/final-qa/`。
  输出: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

- [x] F4. **范围保真度检查** — `deep`
  对每个 task：读 "What to do"，读实际 diff。验证 1:1 — spec 中的都已构建，spec 外的没有。检查 "Must NOT do" 合规。检测跨 task 污染。标记未经说明的变更。
  输出: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `investigate(sr): verify EXR channel names and MV direction` | 调查脚本 + 结果 | — |
| 2 | `feat(blender): add Halton jitter to AOV export scripts` | aov_export_preset.py, __init__.py | pytest tests/ |
| 3 | `feat(compositor): read jitter from camera.json and wire to DLSS-SR` | camera_data_loader.cpp/.h, sequence_processor.cpp | ctest --test-dir build |
| 4 | `fix(mv): negate MV X,Y for DLSS curr→prev convention` | mv_converter.cpp, test_mv_converter.cpp | ctest --test-dir build -R mv |
| 5 | `fix(ngx): enable AutoExposure flag for DLSS-SR` | ngx_wrapper.cpp | ctest --test-dir build |
| 6 | `test(e2e): end-to-end verification with all fixes` | 证据文件 | — |

---

## Success Criteria

### Verification Commands
```bash
# C++ 测试
ctest --test-dir build --output-on-failure  # Expected: all tests pass

# Python 测试
pytest tests/ -v  # Expected: all tests pass

# Compositor 运行（验证 jitter 生效）
dlss-compositor.exe --input-dir <jittered_frames> --output-dir <output> --scale 2 --preset L --verbose 2>&1 | findstr "jitter"
# Expected: 每帧显示非零 jitter offset

# EXR 通道检查
python -c "import OpenEXR; f=OpenEXR.InputFile('test_frame.exr'); print(sorted(f.header()['channels'].keys()))"
# Expected: 包含 Vector.X, Vector.Y, Depth.Z 等通道
```

### Final Checklist
- [ ] 所有 "Must Have" 存在
- [ ] 所有 "Must NOT Have" 不存在
- [ ] 所有 C++ 和 Python 测试通过
- [ ] DLSS-SR 输出质量有视觉可感知提升
- [ ] 时域稳定性良好（无鬼影/闪烁）
