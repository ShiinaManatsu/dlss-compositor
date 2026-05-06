# Phase 2 — SR Quality Improvements (Core Wins)

## TL;DR

> **目标**: 实质性提升 DLSS-SR 输出质量。删除已证实"研究用途"的 GBuffer 路径（省 30%+ 渲染/IO，零质量损失）；新增 post-DLSS sharpen pass（补"游戏味"）；按 NVIDIA 推荐设置 per-quality preset 默认值；启动 banner 让 GUI 可读取版本信息。
>
> **交付物**:
> - 删除 GBuffer 路径（DiffCol / GlossCol / Normal / Roughness AOV 全删）
> - 新增 CAS 锐化 compute shader + processor + `--sharpness` flag
> - Per-quality-mode preset 默认值（DLAA/Quality/Balanced=K，Performance=M，UltraPerformance=L），支持用户 override
> - 启动 banner 输出 SDK 版本、DLL 版本、preset 选择，GUI 正则可解析
>
> **预估工作量**: L（核心改动 + 新 shader + 跨 Blender/C++）
> **并行执行**: YES — 3 波次
> **关键路径**: GBuffer 删除（影响最广）→ Sharpen + Banner（独立）→ 综合验证

**前置**: Phase 1 完成（CLI 已扩展、jitter 已修）

---

## Context

### Original Request
经过今天对 NGX SDK header、Streamline 文档、nvpro-samples 的三方验证，确认：
1. **DLSS-SR 不消费 GBuffer**——header 注释明说"OPTIONAL - only for research purposes"，Streamline 文档不列为输入，官方 vk_streamline 样例只 tag 4 个资源（Color/Depth/MV/Output）
2. **GlossCol 即使在我们当前路径里也是死数据**——读了但没传给 DLSS
3. **DLSS 输出固有偏软**——游戏跑 CAS/Unsharp 隐藏，离线工具不加 → 用户感知"不够锐"
4. **Per-quality preset 同值是 anti-pattern**——NVIDIA 推荐不同 quality 用不同 preset

### Background
- 用户典型场景：4K 30fps 视频，源渲染 50% (1920×1080) 或 75% (2880×1620)，SR 上采到 4K，然后 FG 插帧到 60/120
- Cycles 预降噪开启（已确认）
- 当前管线 OK 但有改进空间

### 已确认决策
- ✅ 删除 GBuffer（NVIDIA 三方证据 + 用户已批准）
- ✅ Sharpen 用 CAS 算法（FFX 开源）
- ✅ Per-quality preset 加 `Auto` 默认（按 NVIDIA 推荐分配）
- ❌ 不切到 Streamline（NGX 直通保持）
- ❌ 不重启 RR 路径

---

## Work Objectives

### Core Objective
让 DLSS-SR 输出质量在用户感知上明显接近游戏 DLSS 体验：去掉无效输入（更快），补上锐化（更清晰），用对 preset（更适配）。

### Concrete Deliverables
- **Blender 端**: 移除 4 个 pass 配置 + Roughness AOV + Principled BSDF AOV 节点逻辑
- **C++ 删除**: ChannelMapper 4 个字段、ExrReader 4 个 alias、DlssSRFrameInput 6 个字段、SR processor 中 GBufferSurface 填充
- **C++ 新增**:
  - `src/shaders/cas_sharpen.comp`
  - `src/gpu/sharpen_processor.h/cpp`
- **CLI 新增**: `--sharpness <0.0..1.0>` (默认 0.0)
- **Preset 默认**: `DlssSRPreset::Auto` 枚举值，根据 quality 自动分配
- **Banner**: NgxContext::init 输出结构化 banner

### Definition of Done
- [ ] EXR 文件大小比改前小 30-50%
- [ ] Cycles 渲染时间减小 10-20%（可测）
- [ ] `--sharpness 0.3` 输出可见锐化效果
- [ ] `--quality Performance --preset auto` 实际用 Preset M
- [ ] `dlss-compositor.exe --test-ngx` 输出含 `[NGX] SDK: 310.5.3` 等 banner 行
- [ ] 全套测试通过

### Must Have
- 4 个独立 commit（GBuffer 删 / Sharpen / Preset / Banner）
- A/B 验证显示 GBuffer 删除前后无视觉质量差
- Sharpen 默认 0.0（向后兼容）
- 每个 commit 独立可回退

### Must NOT Have (Guardrails)
- ❌ 不修改 SR/FG 核心 NGX 调用流程（仅 GBufferSurface 填充删除是例外）
- ❌ 不修改 channel mapper 的 Color/Depth/MV 读取
- ❌ 不修改 jitter / camera.json 处理
- ❌ 不引入 OIDN / OptiX / RR fallback
- ❌ 不重写 mv_converter / pq_transfer / lut_processor
- ❌ 不修改 Electron GUI 已有功能（可加 sharpness slider，但不重构）
- ❌ 不动 FG 路径的 GBuffer（FG 本来就不传 GBuffer）

---

## Verification Strategy

### Test Decision
- **基础设施**: YES (Catch2, pytest)
- **自动化测试**: YES (TDD for sharpen shader, tests-after for deletions)
- **框架**: 现有

### QA Policy
- **GBuffer 删除验证**: 改前后 EXR 文件大小对比 + 渲染时间对比 + 视觉对比（用户最终确认）
- **Sharpen 验证**: 已知输入图像跑 sharpen，验证输出 contrast 升高（pixel std 上升）
- **Preset 验证**: 各 quality 模式跑一次，verbose log 显示实际 preset
- **Banner 验证**: stdout 正则匹配

证据保存到 `.sisyphus/evidence/phase2/task-{N}-{slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (并行准备 — 互不依赖):
├── Task 1: 删除 GBuffer 路径（Blender 端）
├── Task 2: 删除 GBuffer 路径（C++ 端）
├── Task 3: Per-quality preset Auto 默认
└── Task 4: 启动 banner

Wave 2 (Sharpen 实现):
└── Task 5: CAS sharpen shader + processor + CLI flag

Wave 3 (集成 + e2e):
├── Task 6: A/B 测试（GBuffer 改前/后视觉对比）
└── Task 7: 完整 e2e 验证

Wave FINAL (审查):
├── F1-F4
```

### Dependency Matrix

| Task | Depends On | Blocks | Wave |
|------|-----------|--------|------|
| 1 | — | 6 | 1 |
| 2 | — | 6 | 1 |
| 3 | — | 7 | 1 |
| 4 | — | 7 | 1 |
| 5 | — | 7 | 2 |
| 6 | 1, 2 | 7 | 3 |
| 7 | 3, 4, 5, 6 | F1-F4 | 3 |

### Agent Dispatch Summary

- **Wave 1**: 4 tasks — T1 → `unspecified-high`, T2 → `unspecified-high`, T3 → `quick`, T4 → `quick`
- **Wave 2**: 1 task — T5 → `visual-engineering`（涉及 GLSL）
- **Wave 3**: 2 tasks — T6 → `deep`, T7 → `unspecified-high`
- **FINAL**: F1 → `oracle`, F2-F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

- [ ] 1. 删除 GBuffer 路径（Blender 端）

  **What to do**:
  - **`blender/dlss_compositor_aov/__init__.py`**:
    - 删除 `vl.use_pass_diffuse_color = True`
    - 删除 `vl.use_pass_glossy_color = True`
    - 删除 `vl.use_pass_normal = True`
    - 删除 Roughness AOV 注册（`view_layer.aovs.add()` 等）
    - 删除遍历 Principled BSDF 注入 AOV Output 节点的代码（撤销对用户材质的修改逻辑）
    - 删除 compositor File Output node 中对应 4 个 slot
    - 更新 panel UI 显示（去 4 个 indicator）
  - **`blender/aov_export_preset.py`**：同步同样的删除
  - **撤销旧场景中的注入**:
    - 提供一个新 operator `dlsscomp.cleanup_legacy_aov_nodes`（菜单项 "Remove DLSS AOV Output Nodes"）让用户从已配置的旧场景里清理残留的 AOV Output 节点
    - 这是用户体验考虑，避免残留垃圾节点
  - 更新 panel 文字描述、bl_info 版本

  **Must NOT do**:
  - 不删除 Combined / Z / Vector pass 配置
  - 不删除 jitter 相关代码
  - 不删除 camera.json 导出
  - 不删除 Roughness AOV 之外的自定义 AOV 支持（如果有用户其他用途——审计后决定）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None

  **References**:
  - `blender/dlss_compositor_aov/__init__.py:208+` 当前 pass 配置
  - `blender/aov_export_preset.py:341` glossy_color 配置
  - 用户已批准（基于 NGX header 三方证据）

  **Acceptance Criteria**:
  - [ ] Blender 配置后渲染 1 帧的 EXR 仅含 Combined / Depth / Vector 通道（用 OpenEXR 工具验证）
  - [ ] 渲染时间减少（与改前 baseline 对比）
  - [ ] 旧场景能用新 operator 清理残留节点

  **QA Scenarios**:
  ```
  Scenario: EXR 通道列表收缩
    Tool: Blender headless + Python OpenEXR
    Steps:
      1. blender --background --factory-startup --python tests/blender/test_minimal_aov.py
      2. python -c "import OpenEXR; f=OpenEXR.InputFile('out.exr'); print(sorted(f.header()['channels'].keys()))"
    Expected Result: 仅 Image.RGBA + Depth.Z + Vector.XYZW（不再有 DiffCol/GlossCol/Normal/Roughness）
    Evidence: .sisyphus/evidence/phase2/task-1-channels.txt

  Scenario: 旧场景清理 operator 工作
    Tool: Blender headless
    Steps:
      1. 加载预先准备的"旧场景 with AOV nodes" fixture
      2. 调用 bpy.ops.dlsscomp.cleanup_legacy_aov_nodes()
      3. 检查所有材质 node tree 不再有 ShaderNodeOutputAOV with name="Roughness"
    Expected Result: 节点已清理
    Evidence: .sisyphus/evidence/phase2/task-1-cleanup.txt
  ```

  **Commit**: YES
  - Message: `refactor(blender): remove DLSS GBuffer passes (verified unused per NVIDIA spec)`
  - Files: `blender/dlss_compositor_aov/__init__.py, blender/aov_export_preset.py, blender/dlss_compositor_aov/blender_manifest.toml (version bump)`
  - Pre-commit: 手动 Blender 测试

- [ ] 2. 删除 GBuffer 路径（C++ 端）

  **What to do**:
  - **`src/core/exr_reader.cpp:33-62`**:
    - 删除 `Diffuse Color.* → RenderLayer.DiffCol.*` alias
    - 删除 `Glossy Color.* → RenderLayer.GlossCol.*` alias
    - 删除 `Normal.* → RenderLayer.Normal.*` alias
    - 删除 `Roughness.V → RenderLayer.Roughness.X` alias
  - **`src/core/channel_mapper.h:19-39`** 和 **`.cpp`**:
    - 从 `ChannelNames` 删除 diffR/G/B、specR/G/B、normalX/Y/Z、roughness 字段
    - 从 `MappedBuffers` 删除 `diffuseAlbedo`, `specularAlbedo`, `normals`, `roughness` 字段（vector<float>）
    - `mapFromExr` 函数对应的读取代码块删除（约 line 81-111）
    - 移除"defaulted to white/black/+Z/0.5"的 fallback 逻辑（不再需要）
  - **`src/dlss/dlss_sr_processor.h`**:
    - 从 `DlssSRFrameInput` 删除 6 个字段（diffuseAlbedo/diffuseView/normals/normalsView/roughness/roughnessView）
  - **`src/dlss/dlss_sr_processor.cpp:121-179`**:
    - 删除 albedo/normals/roughness 的 `validateImage` 调用
    - 删除 `albedoResource/normalsResource/roughnessResource` 局部变量
    - 删除 `evalParams.GBufferSurface.pInAttrib[*]` 三处填充
  - **`src/pipeline/sequence_processor.cpp:763-806`**:
    - 删除 GBuffer 纹理分配（`texturePool->acquire(... diffuseAlbedo ...)` 等）
    - 删除 frame.diffuseAlbedo/normals/roughness 赋值
  - **`tests/test_channel_mapper.cpp`**:
    - 删除 albedo/normals/roughness 相关测试用例
    - 简化 fixture EXR 为仅 Combined/Depth/Vector
  - **`tests/test_dlss_*.cpp`**:
    - 移除测试中对 GBuffer 字段的引用
  - **`tools/exr_validator.py`**:
    - 更新 required/optional 通道列表
    - DiffCol/GlossCol/Normal/Roughness 从 optional 列表移除（或加注释说"已废弃"）

  **Must NOT do**:
  - 不删除 Color / Depth / MV 通道处理
  - 不删除 jitter / camera 处理
  - 不删除 mv_converter
  - 不修改 NGX_VULKAN_EVALUATE_DLSS_EXT 调用语义（只是不再传 GBufferSurface）
  - 不动 FG processor

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None（可与 Task 1 并行做，但最终 e2e 测试需两边都完成）

  **References**:
  - 三方证据：`DLSS/include/nvsdk_ngx_helpers_vk.h:89-90` "research purposes only"
  - vk_streamline `main.cpp:324-329` 仅 4 个 ResourceTag

  **Acceptance Criteria**:
  - [ ] `grep -r "diffuseAlbedo\|specularAlbedo\|GBufferSurface" src/` 无生产代码匹配
  - [ ] `ctest` 全过
  - [ ] GPU 内存使用减小（通过日志观察 texture pool 大小）

  **QA Scenarios**:
  ```
  Scenario: GBuffer 引用已清除
    Tool: Grep
    Steps:
      1. Select-String -Path "src\**\*.cpp","src\**\*.h" -Pattern "GBufferSurface\.pInAttrib|diffuseAlbedo|specularAlbedo" -SimpleMatch:$false
    Expected Result: 0 matches in src/dlss/dlss_sr_processor.* (FG 不动)
    Evidence: .sisyphus/evidence/phase2/task-2-gbuffer-removed.txt

  Scenario: 测试无 GBuffer 假设
    Tool: Bash (ctest)
    Steps:
      1. cmake --build build --config Release
      2. ctest --test-dir build --output-on-failure
    Expected Result: 全过
    Evidence: .sisyphus/evidence/phase2/task-2-tests.txt
  ```

  **Commit**: YES
  - Message: `refactor(dlss): remove SR GBuffer feeding (NGX header marks "research only")`
  - Files: `src/core/exr_reader.cpp, src/core/channel_mapper.*, src/dlss/dlss_sr_processor.*, src/pipeline/sequence_processor.cpp, tests/*, tools/exr_validator.py`
  - Pre-commit: `ctest --test-dir build`

- [ ] 3. Per-Quality Preset Auto 默认值

  **What to do**:
  - **`src/cli/config.h`**:
    - `DlssSRPreset` 枚举新增 `Auto` 值（推荐排在第一位作为默认）
    - 默认 `DlssSRPreset preset = DlssSRPreset::Auto;`
  - **`src/cli/cli_parser.cpp`**:
    - `--preset auto` 解析为 `Auto`
    - `--preset J/K/L/M` 解析为对应 enum
    - `--help` 描述更新："auto (default, NVIDIA-recommended per quality) | J | K | L | M"
  - **`src/dlss/ngx_wrapper.cpp:181-187`**:
    ```cpp
    auto pickPreset = [&](DlssSRPreset def) {
        return preset == DlssSRPreset::Auto ? static_cast<int>(def) : static_cast<int>(preset);
    };
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pickPreset(DlssSRPreset::K));
    NVSDK_NGX_Parameter_SetI(..._Quality, pickPreset(DlssSRPreset::K));
    NVSDK_NGX_Parameter_SetI(..._Balanced, pickPreset(DlssSRPreset::K));
    NVSDK_NGX_Parameter_SetI(..._Performance, pickPreset(DlssSRPreset::M));
    NVSDK_NGX_Parameter_SetI(..._UltraPerformance, pickPreset(DlssSRPreset::L));
    NVSDK_NGX_Parameter_SetI(..._UltraQuality, pickPreset(DlssSRPreset::K));
    ```
  - 在 ngx_wrapper 创建 feature 时，verbose log 输出"effective preset"（如 `preset=K (auto for Quality)`）
  - 更新测试覆盖 Auto + 各 quality 的组合

  **Must NOT do**:
  - 不破坏现有 explicit preset override（用户传 `--preset L` 仍生效）
  - 不修改 quality mapping (mapQuality 函数)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: None

  **References**:
  - `src/dlss/ngx_wrapper.cpp:181-187` 当前同 preset 逻辑
  - NVIDIA 推荐：DLAA/Q/B = K, P = M, UP = L

  **Acceptance Criteria**:
  - [ ] `--quality Performance --preset auto` 实际用 Preset M（log 验证）
  - [ ] `--quality Performance --preset K` 强制 K（override 生效）
  - [ ] 默认（无 preset flag）= Auto

  **QA Scenarios**:
  ```
  Scenario: Auto 按推荐分配
    Tool: Bash + log
    Steps:
      1. for q in Performance, Balanced, MaxQuality: 跑 --quality $q --preset auto --ngx-log-level info
      2. 解析 stderr 确认 preset 字母
    Expected Result: P=M, B=K, MaxQuality=K
    Evidence: .sisyphus/evidence/phase2/task-3-auto-preset.txt

  Scenario: Override 生效
    Tool: Bash
    Steps:
      1. --quality Performance --preset L --ngx-log-level info
      2. 解析 log
    Expected Result: preset=L (user override)
    Evidence: .sisyphus/evidence/phase2/task-3-override.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): add Auto preset default per NVIDIA quality recommendations`
  - Files: `src/cli/*, src/dlss/ngx_wrapper.cpp, tests/test_cli.cpp`
  - Pre-commit: `ctest`

- [ ] 4. 启动 Banner（GUI 可正则解析）

  **What to do**:
  - **`src/dlss/ngx_wrapper.cpp NgxContext::init`** 成功后输出结构化 banner：
    ```cpp
    Log::info("[NGX] SDK: %s (compile-time)\n", appver::kDlssSdkVersion);
    Log::info("[NGX] Engine: %s %s\n", appver::kEngineName, appver::kEngineVersion);
    
    if (m_dlssSRAvailable) {
        Log::info("[NGX] DLSS-SR: available\n");
    } else {
        Log::info("[NGX] DLSS-SR: unavailable (%s)\n", m_unavailableReason.c_str());
    }
    
    if (m_dlssFGAvailable) {
        Log::info("[NGX] DLSS-FG: available, max-multi-frame=%d\n", m_maxMultiFrameCount);
    } else {
        Log::info("[NGX] DLSS-FG: unavailable\n");
    }
    
    int driverMajor = 0, driverMinor = 0;
    if (readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, driverMajor)
     && readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, driverMinor)) {
        Log::info("[NGX] Min driver: %d.%d\n", driverMajor, driverMinor);
    }
    ```
  - **DLL 文件版本查询** (Windows-specific):
    - 用 `LoadLibraryW` + `GetProcAddress` + `GetModuleFileNameW` + `GetFileVersionInfoW` 读 nvngx_dlss.dll 和 nvngx_dlssg.dll 的实际版本
    - 输出 `[NGX] Loaded: nvngx_dlss.dll v<file_ver>, nvngx_dlssg.dll v<file_ver>`
    - 注意：DLL 可能由驱动注入更新版本，所以 file version 比 SDK 编译版本更准确
  - **`createDlssSR` 成功后**输出：
    ```
    [NGX] DLSS-SR feature created: 1920x1080 → 3840x2160, quality=MaxQuality, preset=K (auto)
    ```
  - **GUI 端**: 提供正则供 Electron GUI 解析（不在本 task 改 GUI，仅文档化）：
    ```
    \[NGX\] SDK: (\S+)
    \[NGX\] DLSS-(SR|FG): (available|unavailable)
    \[NGX\] DLSS-SR feature created: (\d+)x(\d+) → (\d+)x(\d+), quality=(\w+), preset=(\w+)
    ```

  **Must NOT do**:
  - 不修改 NGX init 调用顺序
  - 不强制改 log level（保持用户配置的级别——但 banner 用 info 级别输出）
  - 不让 banner 输出到 stderr（应是 stdout 或专门的 info channel）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**: YES (Wave 1)
  **Blocked By**: Phase 1 Task 1（version.h 必须先合并）

  **References**:
  - `src/dlss/ngx_wrapper.cpp:115-133` init 流程
  - `src/core/version.h` (Phase 1 创建)
  - Windows GetFileVersionInfo API

  **Acceptance Criteria**:
  - [ ] `--test-ngx` 输出至少 4 行 `[NGX]` 开头的 banner
  - [ ] DLL 版本能查询到（在 Windows 上）
  - [ ] 正则可解析（脚本测试）

  **QA Scenarios**:
  ```
  Scenario: Banner 完整输出
    Tool: Bash
    Steps:
      1. build\Release\dlss-compositor.exe --test-ngx > banner.txt 2>&1
      2. findstr /c:"[NGX] SDK:" banner.txt
      3. findstr /c:"[NGX] Engine:" banner.txt
      4. findstr /c:"[NGX] DLSS-SR:" banner.txt
      5. findstr /c:"[NGX] DLSS-FG:" banner.txt
    Expected Result: 4 行全部匹配
    Evidence: .sisyphus/evidence/phase2/task-4-banner.txt

  Scenario: 正则解析（GUI 端模拟）
    Tool: Bash + PowerShell regex
    Steps:
      1. $log = Get-Content banner.txt
      2. $log | Select-String -Pattern '\[NGX\] SDK: (\S+)' | % { $_.Matches.Groups[1].Value }
      3. 应得到 "310.5.3"
    Expected Result: 解析成功
    Evidence: .sisyphus/evidence/phase2/task-4-regex.txt
  ```

  **Commit**: YES
  - Message: `feat(ngx): add structured startup banner for GUI/log parsing`
  - Files: `src/dlss/ngx_wrapper.cpp, src/core/version.h (引用), src/core/logger.h (如需新方法)`
  - Pre-commit: `ctest && build\Release\dlss-compositor.exe --test-ngx | findstr /c:"[NGX]"`

- [ ] 5. CAS Sharpen Shader + Processor + CLI flag

  **What to do**:
  - **新建 `src/shaders/cas_sharpen.comp`**:
    - 实现 AMD FFX CAS 单算子（参考开源 CAS GLSL 实现）
    - 或简化版 unsharp mask：`output = mix(input, input + sharpness * (input - blur(input)), strength)`
    - Push constant: `imageWidth, imageHeight, sharpness (0.0-1.0)`
    - Workgroup size: `(16, 16, 1)`
    - 输入：sampler2D（SR 输出，linear HDR R16G16B16A16）
    - 输出：image2D storage（同分辨率）
  - **新建 `src/gpu/sharpen_processor.h/cpp`**:
    - 类似 `pq_transfer_processor` 的结构（pipeline、descriptor set layout、command 录制）
    - `class SharpenProcessor { void create(...); void cmdSharpen(VkCmdBuf, srcImg, dstImg, w, h, sharpness); void destroy(); };`
  - **`src/cli/config.h`**:
    - `float sharpness = 0.0f; // [0.0, 1.0]`
  - **`src/cli/cli_parser.cpp`**:
    - 新增 `--sharpness <float>` flag（解析 + clamp [0.0, 1.0]）
    - `--help` 描述："Post-DLSS sharpening (default 0.0 = off, recommended 0.2-0.4)"
  - **`src/pipeline/sequence_processor.cpp`**:
    - SR 路径：DLSS evaluate 后、写 EXR 前（或 PQ encode 前），如果 `config.sharpness > 0.0`，调用 `sharpenProcessor.cmdSharpen(...)`
    - 注意 image layout transitions（SHADER_READ_ONLY ↔ GENERAL）
    - 注意 in-place 还是 ping-pong（建议 ping-pong：临时 sharpened buffer → swap → 后续流程）
  - **CMakeLists**:
    - 编译 cas_sharpen.comp 为 SPIR-V 并嵌入资源
    - 添加 sharpen_processor.cpp 到 sources

  **Must NOT do**:
  - 不在 sharpen 之前/之后跳过 PQ 编码（如果 FG 路径用到）
  - 不实现高级算法（multi-pass、edge-aware 等）—— 简单 CAS 足够
  - 不强制开启（默认 0.0 = bypass）
  - 不修改 SR 输出 buffer 的格式

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: []

  **Parallelization**: YES (Wave 2)
  **Blocked By**: None（可与 Wave 1 并行做，但合并到 Wave 2 让 review 更聚焦）

  **References**:
  - AMD FFX CAS 公开实现：https://github.com/GPUOpen-Effects/FidelityFX-CAS
  - 我们的 `src/gpu/pq_transfer_processor.cpp` 作为相似 processor 参考
  - `src/shaders/pq_transfer.comp` 作为 compute shader 模板

  **Acceptance Criteria**:
  - [ ] `--sharpness 0.0` 输出与 SR 直出一致（对比 pixel 一致）
  - [ ] `--sharpness 0.5` 输出 std deviation 比 SR 直出高（量化锐化效果）
  - [ ] Shader compile 无 warning
  - [ ] 单元测试覆盖 sharpen processor 的 init/destroy

  **QA Scenarios**:
  ```
  Scenario: sharpness=0 旁路（pixel 一致）
    Tool: Bash + Python
    Steps:
      1. 跑序列 --sharpness 0.0，输出到 dir_a/
      2. 跑序列 --sharpness 0.0（重复），输出到 dir_b/
      3. python -c "import OpenEXR; ... compare dir_a/frame_0001.exr vs dir_b/frame_0001.exr"
    Expected Result: 完全一致（确认 sharpen=0 真的旁路）
    Evidence: .sisyphus/evidence/phase2/task-5-bypass.txt

  Scenario: sharpness > 0 增加 contrast
    Tool: Bash + Python
    Steps:
      1. 跑序列 --sharpness 0.0 → dir_baseline/
      2. 跑序列 --sharpness 0.5 → dir_sharp/
      3. python: 计算两组输出 frame 的 std deviation
    Expected Result: dir_sharp 平均 std > dir_baseline std
    Evidence: .sisyphus/evidence/phase2/task-5-contrast.txt

  Scenario: 单元测试
    Tool: Bash (ctest)
    Steps:
      1. ctest --test-dir build -R sharpen
    Expected Result: 通过
    Evidence: .sisyphus/evidence/phase2/task-5-unit.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): add CAS sharpen pass with --sharpness flag`
  - Files: `src/shaders/cas_sharpen.comp, src/gpu/sharpen_processor.*, src/cli/*, src/pipeline/sequence_processor.cpp, CMakeLists.txt, tests/test_sharpen_processor.cpp`
  - Pre-commit: `cmake --build build && ctest -R sharpen`

- [ ] 6. A/B 视觉验证（GBuffer 删除前 vs 后）

  **What to do**:
  - 选取一段代表性 Blender 序列（如 SnowMix 已有的 4k_aov 16 帧）
  - **改前 baseline**:
    - checkout 上一个 commit (在 Task 1+2 之前)
    - 渲染（用旧 Blender 配置含 GBuffer）
    - 跑 compositor 处理 → 输出到 `baseline_with_gbuffer/`
  - **改后**:
    - checkout 当前（GBuffer 已删）
    - 渲染（用新 Blender 配置无 GBuffer）
    - 跑 compositor → 输出到 `current_no_gbuffer/`
  - **对比方式**:
    - 选取若干帧（如帧 1, 5, 10, 15）
    - 用 Python (PIL / OpenEXR) 计算 PSNR / SSIM
    - 生成并排对比图
    - 同时记录：
      - 渲染时间对比
      - EXR 文件大小对比
      - GPU 内存峰值对比
  - **用户视觉确认**:
    - 把对比图保存为证据
    - 提供给用户看，确认"无视觉质量差异"
  - 如果用户发现质量下降（不该发生但要预案）→ 触发回滚预案

  **Must NOT do**:
  - 不修改任何代码
  - 不在小测试样本上下结论（必须是真实场景，至少 8-16 帧）

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: [`playwright`] — 如需要截图对比展示

  **Parallelization**: NO (Wave 3)
  **Blocked By**: Tasks 1, 2

  **References**:
  - 用户的 `E:\Render Output\Compositing\SnowMix\sequences\4k_aov` 序列
  - 或新建一个简单测试场景（用 fixtures）

  **Acceptance Criteria**:
  - [ ] PSNR > 40 dB（实际无差异）
  - [ ] SSIM > 0.99
  - [ ] 渲染时间减小可量化
  - [ ] EXR 大小减小 30%+
  - [ ] 用户确认视觉无差

  **QA Scenarios**:
  ```
  Scenario: 量化对比
    Tool: Bash + Python
    Steps:
      1. python tools/ab_compare.py baseline_with_gbuffer/ current_no_gbuffer/ --frames 1,5,10,15
      2. 输出 PSNR / SSIM 表
      3. 渲染时间和文件大小统计
    Expected Result: PSNR 高、SSIM 高、size/time 减小
    Evidence: .sisyphus/evidence/phase2/task-6-ab-metrics.txt

  Scenario: 视觉确认（用户）
    Tool: Manual
    Steps:
      1. 把对比图发给用户
      2. 用户视觉判断
    Expected Result: 用户确认"无肉眼可见差异"
    Evidence: .sisyphus/evidence/phase2/task-6-user-ok.txt
  ```

  **Commit**: NO（仅验证）

- [ ] 7. 完整 e2e 验证

  **What to do**:
  - 跑全套测试（C++ ctest + Python pytest）
  - 跑真实 Blender 序列 → SR-only → SR+FG → 视觉确认
  - 验证 banner 在 e2e 中正确显示
  - 验证 sharpness 在 e2e 中可调
  - 验证 preset auto 在 e2e 中正确分配
  - 二进制大小 / GPU 内存使用 / 渲染时间记录

  **Must NOT do**:
  - 不修改代码
  - 不跳过任何失败测试

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**: NO (Wave 3)
  **Blocked By**: Tasks 3, 4, 5, 6

  **Acceptance Criteria**:
  - [ ] 全套测试通过
  - [ ] 真实序列处理无错
  - [ ] 用户视觉确认 SR 质量"接近游戏味"

  **QA Scenarios**:
  ```
  Scenario: 完整 e2e
    Tool: Bash
    Steps:
      1. ctest --test-dir build --output-on-failure
      2. pytest tests/ -v
      3. blender + compositor 真实序列
      4. 用 --sharpness 0.3 + --preset auto + --quality MaxQuality 跑一次
      5. 对比直出
    Expected Result: 全部成功，质量提升明显
    Evidence: .sisyphus/evidence/phase2/task-7-e2e.txt
  ```

  **Commit**: NO（验证 only）

---

## Final Verification Wave

- [ ] F1. **计划合规审计** — `oracle`
  Must Have / Must NOT Have grep 验证。Evidence 文件齐全。

- [ ] F2. **代码质量审查** — `unspecified-high`
  Build + 测试 + 无 stale GBuffer 引用。

- [ ] F3. **真实 QA** — `unspecified-high`
  完整 e2e + A/B 对比报告 + 用户确认。

- [ ] F4. **范围保真度检查** — `deep`
  Diff 范围与 Concrete Deliverables 一致，FG 路径未被误改。

---

## Commit Strategy

| Commit | Message | Files | Pre-commit |
|--------|---------|-------|------------|
| 1 | `refactor(blender): remove DLSS GBuffer passes (verified unused per NVIDIA spec)` | blender/* | manual blender test |
| 2 | `refactor(dlss): remove SR GBuffer feeding (NGX header marks "research only")` | src/core/exr_reader.cpp, src/core/channel_mapper.*, src/dlss/dlss_sr_processor.*, src/pipeline/sequence_processor.cpp, tests/*, tools/exr_validator.py | ctest |
| 3 | `feat(dlss): add Auto preset default per NVIDIA quality recommendations` | src/cli/*, src/dlss/ngx_wrapper.cpp, tests/test_cli.cpp | ctest |
| 4 | `feat(ngx): add structured startup banner for GUI/log parsing` | src/dlss/ngx_wrapper.cpp, src/core/version.h | ctest |
| 5 | `feat(gpu): add CAS sharpen pass with --sharpness flag` | src/shaders/cas_sharpen.comp, src/gpu/sharpen_processor.*, src/cli/*, src/pipeline/sequence_processor.cpp, CMakeLists.txt, tests/test_sharpen_processor.cpp | ctest -R sharpen |

---

## Success Criteria

### Verification Commands
```bash
ctest --test-dir build --config Release --output-on-failure
pytest tests/ -v

# GBuffer 已删
findstr /s /i "GBufferSurface.pInAttrib" src\dlss\*.cpp  # Expected: 0
findstr /s /i "use_pass_glossy_color" blender\*.py  # Expected: 0

# Sharpen 工作
dlss-compositor.exe --sharpness 0.5 ...  # 输出更锐

# Banner 完整
dlss-compositor.exe --test-ngx 2>&1 | findstr /c:"[NGX]"  # 至少 4 行

# Preset Auto
dlss-compositor.exe --quality Performance --preset auto --ngx-log-level info ... | findstr /c:"preset=M"
```

### Final Checklist
- [ ] 5 个独立 commit 全过
- [ ] PSNR > 40 dB (GBuffer 删前后)
- [ ] EXR 大小减小 30%+
- [ ] 用户确认 sharpen 输出效果好
- [ ] 用户确认 SR 质量提升
