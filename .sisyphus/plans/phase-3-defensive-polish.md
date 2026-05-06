# Phase 3 — Defensive & Tooling Polish

## TL;DR

> **目标**: 一组防御性改进 + 开发工具：矩阵 layout 验证测试（FG 健康检查）、Jitter 范围 clamp + 警告、PQ LUT 生成器脚本（恢复 Custom tonemap 的可用性）。
>
> **交付物**:
> - `tests/test_matrix_layout.cpp`：用静止相机帧验证 `clipToPrevClip ≈ identity`
> - Jitter 防御性 clamp 到 `[-0.5, +0.5]` + 越界警告
> - `tools/generate_pq_lut.py`：生成 PQ forward + inverse LUT 资产
> - 文档化 LUT 生成 + 使用流程
>
> **预估工作量**: S-M（无大改动，多为测试 + 工具）
> **并行执行**: YES — 全部独立
> **关键路径**: 无

**前置**: Phase 0/1/2 已完成（GBuffer 已删，CLI custom tonemap 已可解析）

---

## Context

### Original Request
今日讨论中识别出 3 个非紧急但有价值的项：矩阵 layout 验证（FG 隐患）、jitter 防御编码、LUT 生成器（让 README 宣传的功能真的能用）。

### Background
- **矩阵 layout**: vk_gltf_renderer 显式 `glm::transpose()`，我们走 row-major 链路看上去对，但 `MatrixUtil::multiply` 内部实现需要验证。FG "用得还行" 暗示大概率正确，但应有自动测试守住。
- **Jitter clamp**: NGX 文档说 `InJitterOffsetX/Y` 应在 `[-0.5, +0.5]`。Halton 公式理论保证，但万一 camera.json 被人手编辑或上游脚本 bug → 应有防御。
- **LUT 生成器**: README 宣传 `--tonemap custom` 接受 forward/inverse 3D LUT。代码路径存在，但项目里**没有**实际 LUT 文件。Phase 1 的 Task 2 已让 CLI 可以解析 `custom`，但用户没工具生成 LUT → 功能无法验证使用。

### 已确认决策
- ✅ 矩阵测试用静止相机 fixture，断言 `clipToPrevClip ≈ I` (epsilon 1e-5)
- ✅ Jitter clamp + warn，不抛异常（保持向后兼容）
- ✅ LUT 生成 = Python NumPy 脚本，输出 64³ 或 32³ 的 PQ forward/inverse `.bin`
- ✅ LUT `.bin` 文件不 commit，只 commit 生成脚本

---

## Work Objectives

### Core Objective
让代码更鲁棒（jitter 防御）、让隐患可测（矩阵 layout）、让宣传的功能可用（LUT 生成器）。

### Concrete Deliverables
- **`tests/test_matrix_layout.cpp`**: 新增矩阵 layout 单元测试
- **`tests/fixtures/static_camera.json`**: 静止相机 fixture（手工或脚本生成）
- **`src/pipeline/sequence_processor.cpp`**: jitter clamp + warn
- **`tools/generate_pq_lut.py`**: PQ LUT 生成器（NumPy）
- **`docs/lut_guide.md`** 或 README 段落：LUT 生成 + 使用文档
- **`assets/.gitignore`**（如目录新建）：忽略 `.bin` 文件

### Definition of Done
- [ ] 静止相机 2 帧 → `clipToPrevClip` 与单位矩阵 max-element-diff < 1e-5
- [ ] camera.json 中 jitter > 0.5 时 stderr 出现警告
- [ ] `python tools/generate_pq_lut.py --output assets/pq_forward.bin` 生成正确文件
- [ ] CLI `--tonemap custom --tonemap-lut assets/pq_forward.bin --inverse-tonemap-lut assets/pq_inverse.bin` 跑通
- [ ] 用 PQ LUT 处理一段 FG 序列，输出与 `--tonemap pq` 在视觉上等价

### Must Have
- 3 个独立 commit
- 矩阵测试在 RTX/非 RTX 上都能跑（纯 CPU 数学，无 GPU 依赖）
- LUT 生成器无外部依赖（仅 numpy + struct）
- 防御性 clamp 不破坏向后兼容（log warn 但仍处理）

### Must NOT Have (Guardrails)
- ❌ 不修改 camera_data_loader 解析逻辑（jitter clamp 只在 sequence_processor 应用前做）
- ❌ 不修改 LUT processor C++ 代码（生成器是 Python 端，C++ 端 reader 已存在）
- ❌ 不引入新的 C++ 依赖
- ❌ 不修改 pq_transfer_processor（LUT 是 PQ 的 alternative，不替换）
- ❌ 不要求 MatrixUtil 重写（如发现 bug，开新 task；本 phase 只验证）
- ❌ 不修改 SR/FG 核心 eval 逻辑

---

## Verification Strategy

### Test Decision
- **基础设施**: YES (Catch2, pytest)
- **自动化测试**: YES (TDD for matrix test, tests-after for clamp/LUT)
- **框架**: 现有

### QA Policy
- **矩阵测试**: 纯 CPU 单元测试，跑 ctest 即可
- **Clamp**: 用越界 fixture camera.json 触发警告，抓 stderr
- **LUT 生成器**: 生成 → 解析回来 → 验证数值
- **LUT e2e**: 用 Python 写个 reference 实现对比 LUT 输出 vs PQ 函数输出

证据保存到 `.sisyphus/evidence/phase3/task-{N}-{slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (全部并行):
├── Task 1: 矩阵 layout 验证测试 (S)
├── Task 2: Jitter clamp + 警告 (XS)
└── Task 3: PQ LUT 生成器脚本 (M)

Wave FINAL (审查):
├── F1-F4
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | — | 1 |
| 2 | — | — | 1 |
| 3 | — | — | 1 |

### Agent Dispatch Summary

- **Wave 1**: 3 tasks — T1 → `quick`, T2 → `quick`, T3 → `unspecified-low`
- **FINAL**: F1 → `oracle`, F2-F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [ ] 1. 矩阵 Layout 验证测试

  **What to do**:
  - **新建 `tests/fixtures/static_camera.json`**:
    - 手工编写或用 Blender 脚本生成
    - 包含 2 帧（如 200, 201），相机完全静止（matrix_world 完全相同）
    - 包含 projection 矩阵
    - 不包含 jitter（或 jitter=0）
    - 文件应小（KB 级别）
  - **新建 `tests/test_matrix_layout.cpp`** (Catch2):
    ```cpp
    TEST_CASE("Matrix layout: static camera produces identity reprojection", "[camera][matrix]") {
        CameraDataLoader loader;
        std::string err;
        REQUIRE(loader.load("tests/fixtures/static_camera.json", err));
        
        DlssFgCameraParams params{};
        REQUIRE(loader.computePairParams(201, 200, params, err));
        
        // clipToPrevClip 应近似单位矩阵
        const float tol = 1e-5f;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                float actual = params.clipToPrevClip[r][c];
                INFO("clipToPrevClip[" << r << "][" << c << "] = " << actual << ", expected " << expected);
                REQUIRE(std::abs(actual - expected) < tol);
            }
        }
        
        // prevClipToClip 也应近似单位矩阵（互逆）
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float expected = (r == c) ? 1.0f : 0.0f;
                REQUIRE(std::abs(params.prevClipToClip[r][c] - expected) < tol);
            }
        }
    }
    
    TEST_CASE("Matrix layout: identity world matrix preserves layout", "[camera][matrix]") {
        // 加载已知矩阵 → MatrixUtil::inverse → MatrixUtil::multiply → 验证回到单位
        // 这测 MatrixUtil 内部一致性
    }
    ```
  - 加入 CMake test target
  - 文档：在 test 顶部注释说明 "如果失败，说明 MatrixUtil::multiply 或 inverse 有 layout 假设错位 → 触发 FG 鬼影/反向运动 bug"

  **Must NOT do**:
  - 不修改 MatrixUtil 实现
  - 不修改 camera_data_loader 逻辑
  - 不依赖 GPU（纯 CPU）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None

  **References**:
  - `src/core/camera_data_loader.cpp:269` `computePairParams`
  - `src/core/camera_data_loader.cpp:50-99` `MatrixUtil` 函数
  - vk_gltf_renderer 的 transpose 模式作为对比

  **Acceptance Criteria**:
  - [ ] 测试通过 → MatrixUtil 实现正确
  - [ ] 测试失败 → 揭示具体 layout bug 位置（注释会引导修复）
  - [ ] 测试在 RTX 和非 RTX 都能跑（纯 CPU）

  **QA Scenarios**:
  ```
  Scenario: 静止相机 → 单位 reprojection
    Tool: Bash (ctest)
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --config Release -R matrix
    Expected Result: 全部测试通过
    Failure Indicators: 任何 element-wise diff > 1e-5 → MatrixUtil 有 bug
    Evidence: .sisyphus/evidence/phase3/task-1-matrix.txt

  Scenario: 测试可在无 GPU 环境跑
    Tool: Bash (ctest, GPU disabled)
    Steps:
      1. ctest --test-dir build --config Release -R matrix --output-on-failure
    Expected Result: 不 SKIP（应是纯 CPU 测试）
    Evidence: .sisyphus/evidence/phase3/task-1-cpu-only.txt
  ```

  **Commit**: YES
  - Message: `test(camera): verify matrix layout via static-camera reprojection identity`
  - Files: `tests/test_matrix_layout.cpp, tests/fixtures/static_camera.json, CMakeLists.txt`
  - Pre-commit: `ctest -R matrix`

- [ ] 2. Jitter Range Clamp + Warning

  **What to do**:
  - **`src/pipeline/sequence_processor.cpp`** SR 路径填充 frame.jitterX/Y 处（约 line 822-833）：
    ```cpp
    if (hasCameraData && cameraLoader.hasFrame(frameInfo.frameNumber)) {
        const auto& camData = cameraLoader.getFrame(frameInfo.frameNumber);
        float jx = camData.jitter_x;
        float jy = camData.jitter_y;
        
        if (std::abs(jx) > 0.5f || std::abs(jy) > 0.5f) {
            Log::warn("[jitter] frame %d: out-of-range jitter (%.3f, %.3f), clamping to [-0.5, +0.5]\n",
                      frameInfo.frameNumber, jx, jy);
        }
        
        frame.jitterX = std::clamp(jx, -0.5f, 0.5f);
        frame.jitterY = std::clamp(jy, -0.5f, 0.5f);
    } else {
        frame.jitterX = 0.0f;
        frame.jitterY = 0.0f;
    }
    ```
  - 同样的 clamp 应用于 SR+FG 路径（如有独立赋值点）
  - 单元测试（如 `tests/test_sequence_processor_jitter.cpp` 或扩展现有）：
    - 用越界 jitter 的 camera.json，验证：
      1. 不崩溃
      2. stderr 有 "[jitter]" 警告
      3. 实际传给 DLSS 的值已 clamp

  **Must NOT do**:
  - 不修改 camera_data_loader 解析（loader 仍读原始值，clamp 在 use 处做）
  - 不抛异常（仅 warn）
  - 不丢弃帧

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None

  **References**:
  - `src/pipeline/sequence_processor.cpp:822-833` 当前 jitter 赋值
  - NGX SDK 文档："Jitter offset must be in input/render pixel space, range [-0.5, +0.5]"

  **Acceptance Criteria**:
  - [ ] 越界 jitter (e.g., 0.7) 触发 warn，不崩溃
  - [ ] 实际传 DLSS 的值在 [-0.5, +0.5]
  - [ ] 正常范围内（< 0.5）无 warn 输出

  **QA Scenarios**:
  ```
  Scenario: 越界 jitter 触发警告
    Tool: Bash (ctest 或 e2e)
    Steps:
      1. 准备 fixture camera.json with jitter_x=0.8
      2. cmake --build build && build\Release\dlss-compositor.exe --input-dir ... --camera-data fixture.json 2>&1 | findstr /i "out-of-range jitter"
    Expected Result: 至少 1 行警告匹配
    Evidence: .sisyphus/evidence/phase3/task-2-warn.txt

  Scenario: 正常 jitter 无警告
    Tool: Bash
    Steps:
      1. 用正常 camera.json (jitter in [-0.5, +0.5])
      2. 跑序列，stderr 不应有 "out-of-range"
    Expected Result: 0 匹配
    Evidence: .sisyphus/evidence/phase3/task-2-clean.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): clamp jitter to [-0.5, +0.5] with warning on out-of-range`
  - Files: `src/pipeline/sequence_processor.cpp, tests/* (如新增)`
  - Pre-commit: `ctest`

- [ ] 3. PQ LUT 生成器脚本

  **What to do**:
  - **新建 `tools/generate_pq_lut.py`**:
    - 用 NumPy 实现 PQ (ST 2084) forward 和 inverse 函数
    - 生成 64³ (或 32³) 3D LUT
    - 输出二进制 `.bin` 文件，格式与 `lut_processor.cpp` 期望一致：
      - 文件大小 = lutSize³ × 4 channels × sizeof(float) = e.g., 64³ × 4 × 4 = 4MB for 64³
      - 数据布局：按 X 最快、Y 中、Z 最慢的 row-major 3D 索引
    - CLI: `python tools/generate_pq_lut.py --size 64 --output-dir assets/`
    - 输出 `assets/pq_forward.bin` 和 `assets/pq_inverse.bin`
    - 同时生成 `.json` metadata 描述 LUT 参数（size, shaper range, encoding spec ref）
  - **`assets/.gitignore`**（新建）:
    ```
    *.bin
    !.gitignore
    ```
    - 不 commit 实际 LUT 文件，只 commit 生成器
  - **更新 `requirements.txt`**:
    - 加 `numpy` 如未列
  - **新增 `tests/test_pq_lut_generator.py`** (pytest):
    - 跑 generator → 解析回来 → 数值校验
    - 对比 LUT[encode(x)] ≈ x（roundtrip）
    - 对比 LUT[x] vs CPU PQ 函数输出（精度差 < 1%）
  - **文档**:
    - 新建 `docs/lut_guide.md` 或在 README 加段落：
      - 说明何时用 LUT vs PQ（PQ 是默认；LUT 是 advanced，可自定义）
      - 生成步骤：`python tools/generate_pq_lut.py`
      - 使用：`dlss-compositor.exe --tonemap custom --tonemap-lut assets/pq_forward.bin --inverse-tonemap-lut assets/pq_inverse.bin`
    - 同时记录 PQ 算法引用（ST 2084 标准）

  **Must NOT do**:
  - 不修改 C++ LUT processor 代码
  - 不 commit 生成的 .bin 文件
  - 不引入 OpenColorIO 等重依赖

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: Phase 1 Task 2（CLI custom tonemap 解析必须先工作）

  **References**:
  - `src/gpu/lut_processor.cpp` LUT 文件格式（fileSize == lutSize³ × 4 × sizeof(float)）
  - `src/shaders/pq_transfer.comp` PQ 算法实现（参考公式）
  - ST 2084 标准: m1=2610/16384, m2=2523/4096*128, c1, c2, c3 常数

  **Acceptance Criteria**:
  - [ ] 生成的 .bin 文件大小符合 lut_processor 期望
  - [ ] LUT roundtrip (forward → inverse) 误差 < 1%
  - [ ] LUT 输出 vs CPU PQ 函数误差 < 1%
  - [ ] CLI `--tonemap custom --tonemap-lut <file>` 处理一段序列成功

  **QA Scenarios**:
  ```
  Scenario: LUT 生成 + 文件格式正确
    Tool: Bash (Python)
    Steps:
      1. python tools/generate_pq_lut.py --size 64 --output-dir test_assets/
      2. (Get-Item test_assets/pq_forward.bin).Length  -- 应 = 64*64*64*4*4 = 4194304 bytes
      3. python tests/test_pq_lut_generator.py
    Expected Result: 文件大小匹配；测试通过
    Evidence: .sisyphus/evidence/phase3/task-3-generator.txt

  Scenario: PQ vs LUT 数值精度
    Tool: Bash (Python)
    Steps:
      1. python -c "import numpy; ... 生成 1000 个 sample，对比 LUT 插值 vs CPU PQ 函数"
    Expected Result: 平均误差 < 1%, max 误差 < 5%
    Evidence: .sisyphus/evidence/phase3/task-3-precision.txt

  Scenario: e2e 用 LUT 处理序列
    Tool: Bash
    Steps:
      1. python tools/generate_pq_lut.py --output-dir assets/
      2. build\Release\dlss-compositor.exe --input-dir tests/fixtures/sequence/ --output-dir tmp_lut/ --interpolate 2x --camera-data tests/fixtures/camera.json --tonemap custom --tonemap-lut assets/pq_forward.bin --inverse-tonemap-lut assets/pq_inverse.bin
    Expected Result: 处理成功，输出文件正确
    Evidence: .sisyphus/evidence/phase3/task-3-e2e.txt
  ```

  **Commit**: YES
  - Message: `tools: add PQ LUT generator + docs for --tonemap custom workflow`
  - Files: `tools/generate_pq_lut.py, tests/test_pq_lut_generator.py, docs/lut_guide.md, requirements.txt, assets/.gitignore`
  - Pre-commit: `pytest tests/test_pq_lut_generator.py -v`

---

## Final Verification Wave

- [ ] F1. **计划合规审计** — `oracle`
  Must Have / Must NOT Have 验证。Evidence 文件齐全。

  **QA Scenarios**:
  ```
  Scenario: 全 Must Have 检查
    Tool: Bash
    Steps:
      1. ctest --test-dir build -R matrix  -- 通过
      2. python tools/generate_pq_lut.py --output-dir tmp/ && Test-Path tmp/pq_forward.bin
      3. 用越界 jitter 测试触发警告
    Expected Result: 全部满足
    Evidence: .sisyphus/evidence/phase3/F1.txt
  ```

- [ ] F2. **代码质量审查** — `unspecified-high`
  Build + 全套测试。无 Python lint 错误。

- [ ] F3. **真实 QA** — `unspecified-high`
  3 个 task 各自的 QA 场景全跑。e2e 用 LUT 处理一段序列对比 PQ 输出。

- [ ] F4. **范围保真度检查** — `deep`
  Diff 范围与 Concrete Deliverables 一致。无核心模块改动。

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `test(camera): verify matrix layout via static-camera reprojection identity` | tests/test_matrix_layout.cpp, tests/fixtures/static_camera.json, CMakeLists.txt | ctest -R matrix |
| 2 | `feat(pipeline): clamp jitter to [-0.5, +0.5] with warning on out-of-range` | src/pipeline/sequence_processor.cpp, tests/* | ctest |
| 3 | `tools: add PQ LUT generator + docs for --tonemap custom workflow` | tools/generate_pq_lut.py, tests/test_pq_lut_generator.py, docs/lut_guide.md, requirements.txt, assets/.gitignore | pytest |

---

## Success Criteria

### Verification Commands
```bash
ctest --test-dir build --config Release --output-on-failure
pytest tests/ -v

# Matrix layout 测试
ctest --test-dir build -R matrix

# Jitter clamp（用越界 fixture）
dlss-compositor.exe --camera-data tests/fixtures/jitter_oob.json ... 2>&1 | findstr /c:"out-of-range jitter"

# LUT 生成 + e2e
python tools/generate_pq_lut.py --output-dir assets/
dlss-compositor.exe --tonemap custom --tonemap-lut assets/pq_forward.bin --inverse-tonemap-lut assets/pq_inverse.bin ...
```

### Final Checklist
- [ ] 3 个独立 commit 全过
- [ ] 矩阵测试在所有平台通过
- [ ] LUT e2e 输出与 PQ 输出视觉等价
- [ ] 文档化 LUT workflow

---

## 后续（Phase 4 候选，本次不做）

如果未来要做 Phase 4 架构 polish：
- 引入 `CALL_NGX` 宏（参考 vk_gltf_renderer）
- `NVSDK_NGX_VULKAN_GetFeatureRequirements` 自动扩展发现
- Pipeline barriers 改 stage-specific（去掉 ALL_COMMANDS_BIT）
- Resource enum + 单一 setResource() 重构
- 三条管线（SR / FG / SR+FG）共享 helper 重构

这些都是大改动，建议单独立 plan，不在 Phase 3 里做。
