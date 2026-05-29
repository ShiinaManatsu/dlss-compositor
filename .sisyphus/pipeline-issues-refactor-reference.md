# 管线问题汇总 — 重构参考

**日期**: 2026-05-29  
**基于**: `.sisyphus/plans/` 所有已完成 plan + 本轮审计 `.sisyphus/sr-fg-audit-2026-05-29.md`  
**范围**: 仅 C++ 管线（不含 GUI 和 Blender 插件）

---

## 🔴 Bug 类（影响功能正确性）

### 1. InMVScale 双重缩放 → SR 时间累积失效

| 文件 | `src/pipeline/sequence_processor.cpp` |
|------|---------------------------------------|
| 行号 | 965-966 (SR-only), 1608-1609 (SR+FG) |
| 根因 | `MVLowRes=true` 时 DLSS 内部按 input/output 比例缩放 MV，代码额外乘 `(outputWidth/inputWidth)` → 实际位移 4x → 重投影到错误位置 → 历史无效 → SR ≈ bilinear |
| UE 参照 | `MVScale = (1.0, 1.0)` |
| 修复 | `frame.mvScaleX = 1.0f; frame.mvScaleY = 1.0f;` |
| 影响 | SR-only, SR+FG 的 SR 部分；FG 不受影响（FG 用独立的 `mvecScale = 1/width, 1/height`） |
| 来源 | 本轮审计 |

### 2. pInOutput 每帧填零（浪费，非根因）

| 文件 | `src/pipeline/sequence_processor.cpp:948` |
|------|-------------------------------------------|
| 现状 | 每帧 `texturePool->updateData(output, outputInit.data())` 用零覆盖 DLSS 输出目标 |
| 影响 | 非 SR 失效根因（DLSS 内部管理历史，pInOutput 仅作写目标），但浪费 GPU 带宽（每帧一次 CPU→GPU staging copy） |
| 修复 | 仅在首帧/reset 时初始化，后续跳过 |
| 来源 | 本轮审计 |

### 3. Per-quality preset 同值 bug（Phase 2 Task 3 设计意图未落地）

| 文件 | `src/dlss/ngx_wrapper.cpp` |
|------|---------------------------|
| 现状 | 所有 6 个 quality mode 写入相同的 `presetValue`，无 per-quality 区分 |
| 修复 | 已通过 `dlss-pipeline-fix` plan 修复（`Auto` 映射表：DLAA/Quality/Balanced/UltraQuality=K, Performance=M, UltraPerformance=L） |
| 来源 | Phase 2 / dlss-pipeline-fix |

### 4. readme 虚假信息

| 文件 | `README.md` |
|------|-------------|
| 内容 | 声称 "DLSS-FG internally clamps HDR color buffers to approximately 10 nits" |
| 事实 | 官方 DLSS-FG Programming Guide（310.5.0, 4008 行）中不存在此描述。FG 真正要求：输入必须有 tone mapping + EOTF（display-ready） |
| 修复 | 删除该错误描述，代之以 FG §HDR 的真实要求 |
| 来源 | 本轮审计 |

---

## 🟡 架构/设计问题

### 5. sequence_processor.cpp 过大 (~2300 行)

| 文件 | `src/pipeline/sequence_processor.cpp` |
|------|--------------------------------------|
| 现状 | 三个管线（SR/FG/SRFG）混在同一文件，大量重复代码（depth 转换、MV scale 计算、reset 逻辑各出现 3 次） |
| AGENTS.md | 自身标注 "Refactor candidate to extract shared SR/FG setup into dedicated helpers" |
| 建议 | 提取共享辅助函数：`prepareDepth()`, `prepareMV()`, `computeReset()`, `loadCameraIfNeeded()` |
| 来源 | AGENTS.md / rrfg-pipeline-and-performance |

### 6. FG-only 路径硬依赖 SR 可用性（已修）

| 文件 | `src/main.cpp` |
|------|---------------|
| 现状 | 原代码无条件检查 DLSS-RR 可用性，阻止 FG-only 模式；已修 |
| 来源 | rrfg-pipeline-and-performance |

### 7. 批量上传未实现（单 texture 逐一 submit）

| 文件 | `src/gpu/texture_pipeline.cpp` |
|------|-------------------------------|
| 现状 | 每个 `upload()` 调用 `beginOneTimeCommands()` → copy → `endOneTimeCommands()`，一个 texture 一个 GPU stall |
| 影响 | 大量小同步点，每帧 8+ 次 `vkQueueSubmit` + `vkQueueWaitIdle` |
| 建议 | `uploadBatch()` — 单 command buffer + 单 submit 处理所有 texture |
| 来源 | rrfg-pipeline-and-performance |

### 8. 无 TexturePool 跨帧复用

| 文件 | `src/gpu/texture_pool.h/.cpp`（已实现） |
|------|--------------------------------------|
| 现状 | 已实现但 SR 路径未用 |
| 来源 | rrfg-pipeline-and-performance |

### 9. GBuffer 已完全删除（SR 不需要）

| 范围 | Blender AOV / ChannelMapper / DlssSRFrameInput / ngx_wrapper |
|------|--------------------------------------------------------------|
| 现状 | Phase 2 已删除 DiffCol/GlossCol/Normal/Roughness AOV 及对应的 GBufferSurface 填充 |
| 确认 | `nvsdk_ngx_helpers_vk.h:89-90` 标注 GBuffer "only for research purposes" |
| 来源 | Phase 2 |

---

## 🟢 已确认正确（不做修改）

| 项 | 配置 | 验证来源 |
|---|---|---|
| IsHDR | `true`（默认） | DLSS SR §3.1.2: 线性空间必须 IsHDR=1 |
| MVLowRes | `true`（默认） | DLSS SR §3.6.2: MV 与 input color 同分辨率 = 1 |
| AutoExposure | `true`（默认） | DLSS SR §3.10 |
| DepthInverted | 不设 | 深度是 NDC [0,1]，非反转 |
| MVJittered | 不设 | 离线渲染 MV 不含 jitter（Blender handler 施加 jitter 但不烘焙进 MV） |
| PreviousOutput | 不设 | 可选，UE 也不设；DLSS 内部管理历史 |
| Jitter 传递 | camera.json → InJitterOffset | Blender Halton [-0.5, +0.5] 完整路径 |
| InReset | 仅首帧 + gap + camera cut | 非每帧置 1 |
| MV 符号转换 | `result = -blenderMv` | Blender Vector pass = prev→curr，DLSS 要 curr→prev，取反正确 |
| InFrameTimeDelta | SR: `1000/sourceFps`；FG: 无此字段（用 clipToPrevClip） | 符合 DLSS 文档 |
| SR vs FG 管线顺序 | SR → tonemap → FG | SR 要求 pre-tonemap，FG 要求 post-tonemap+EOTF，顺序正确 |
| CAS 锐化 | 已实现（`src/shaders/cas_sharpen.comp`） | Phase 2 |
| PQ transport (FG) | 已实现（`src/shaders/pq_transfer.comp`） | 理由纠正：不是防 clamp，而是 FG §HDR 要求 display-ready 输入 |
| 相机切检测 | 已实现（位移/旋转阈值） | dlss-pipeline-fix Task 3 |
| NGX API 版本回退 | 已实现 | dlss-pipeline-fix Task 4 |
| DLL 版本 banner | 已实现 | dlss-pipeline-fix Task 4 |
| Per-quality preset CLI | 已实现 | dlss-pipeline-fix Task 2 |
| Advanced NGX flags (all) | 已实现 | dlss-pipeline-fix Task 2 |
| NDJSON progress emitter | 已实现 | phase-1 / dlss-pipeline-fix |
| 帧预取 (async I/O) | 已实现（FramePrefetcher） | rrfg-pipeline-and-performance |

---

## 🔮 重构建议（基于上述问题）

### 优先修复（P0 — 功能正确性）

1. **InMVScale 改 (1,1)**：`sequence_processor.cpp` 两处
2. **pInOutput 跳过每帧清零**：仅首帧/reset 初始化

### 结构改进（P1 — 可维护性）

3. **拆分 sequence_processor.cpp**：提取共享辅助函数，SR/FG/SRFG 各自动用
4. **批量上传 (uploadBatch)**：减少 GPU sync 点
5. **删除 readme 虚假描述**

### 可选优化（P2 — 性能）

6. FG backbuffer 格式改 RGB10A2（FG 文档推荐，可能有硬件加速路径）

---

## 相关文档索引

| 文档 | 路径 |
|------|------|
| DLSS SR 官方文档 | `docs/dlss-refs/DLSS_Programming_Guide_Release.md` |
| DLSS FG 官方文档 | `docs/dlss-refs/DLSS_FG_Programming_Guide.md` |
| NIS 官方文档 | `docs/dlss-refs/NIS_SDK_Programming_Guide.md` |
| 本轮 SR/FG 审计 | `.sisyphus/sr-fg-audit-2026-05-29.md` |
| Phase 2 SR 质量 | `.sisyphus/plans/phase-2-sr-quality.md` |
| 管线修复 plan | `.sisyphus/plans/dlss-pipeline-fix.md` |
| SR 修复 plan | `.sisyphus/plans/dlss-sr-fix.md` |
| FG 集成 plan | `.sisyphus/plans/dlss-frame-generation.md` |
| RR→SR 重构 | `.sisyphus/plans/rr-to-sr-refactor.md` |
| 性能优化 plan | `.sisyphus/plans/rrfg-pipeline-and-performance.md` |
