# DLSS Compositor v2 — 重构目标文档

**日期**: 2026-05-29  
**状态**: 规划中

---

## 总体架构

整套流程由三个部分组成：**Blender 配置插件**、**前端 GUI**、**DLSS CLI**。整体保持此三层架构不变。

```
Blender 插件 (配置 + 导出)  →  dlssconfig.json  →  Flutter GUI (用户配置 + 监控)
                                                        │
                                                        ▼ RPC (JSON-RPC / MethodChannel)
                                                    DLSS CLI (管线执行)
```

---

## 一、Blender 插件 (blender/dlss_compositor_aov)

### 1.1 分辨率 + 颜色空间控制

- 插件面板提供**最终输出分辨率**输入（Width × Height）
- 横向 toggle 单选 **Quality Mode**：Ultra Performance → Performance → Balanced → Quality → DLAA
- **颜色空间**：由 Blender 色彩管理（OCIO）— `display_device` + `view_transform` 共同定义
  - 静默读取 `bpy.context.scene.display_settings.display_device` 和 `view_settings.view_transform`
  - 参考：[Blender 5.1 Color Management — Displays and Views](https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html)
- **EXR 默认输出 scene-linear**（Blender: "Save as Render" 选项仅对 PNG/JPEG 生效，EXR 原始输出不经 view transform）
  - 写入 dlssconfig.json（记录用户的显示意图，管线内部自行处理颜色变换）：
    ```json
    "display_device": "sRGB",
    "view_transform": "ACES 2.0",
    "is_hdr": false,
    ```
  - 管线输入始终为 scene-linear EXR → SR 用 IsHDR=1
  - FG 前按 `is_hdr` / `display_device` 应用对应 EOTF（HDR→PQ, LDR→sRGB）
- 分辨率透传策略：
  - `scene.render.resolution_x/y` = 最终输出分辨率
  - `scene.render.resolution_percentage` = `1 / scale × 100`（如 2x → 50%）
  - scale 由 DLSS quality mode 对应的预设决定（具体值待研究 DLSS 实现文档后确定）
- Jitter phases 按 `1 × scale²` 计算（Blender 输出已为 AA 后结果，base = 1）
  - 2x → 4 相位数
  - 3x → 9 相位数
- 所有具体算法（scale ↔ quality mode 映射、jitter phase base）**待研究 DLSS 实现文档后最终确定**

### 1.2 Pass 配置

- 保留 "Configure All Passes" 按钮（一键配置 Combined + Depth + Vector passes 及 compositor File Output 节点）

### 1.3 Blender Vector Pass 约定（参考）

来自 StackOverflow 验证过的 Blender Cycles Vector pass 行为：

> Vector.X — X displacement from **previous frame to current frame**
> Vector.Y — Y displacement from **previous frame to current frame**
> Values are in **pixels**. X displacement is positive to the **left**, Y displacement is positive **downwards**.

**与 DLSS 文档要求的差异**：

| | Blender Vector Pass | DLSS §3.6 要求 |
|---|---|---|
| 方向 | prev → curr | **curr → prev** |
| 空间 | 像素空间（render resolution） | 像素空间 |
| Y 轴 | +down（top-left origin） | +down（Vulkan top-left origin） |

**转换**：Blender(prev→curr) → 取反 → DLSS(curr→prev)，无需 Y 翻转（两者均为 top-left origin）。MV 以渲染分辨率像素空间存储，MVLowRes=1 告知 DLSS 内部缩放，InMVScale=(1.0,1.0)。

> ⚠️ 上述转换方向为实验结论，Blender 内置文档与社区信息存在矛盾。重构时建议以简单测试场景（物体右移 10px）再次验证符号。

### 1.4 相机数据导出（静默）

- 导出改为 **handler 静默执行**：每次渲染完成后自动写入，无需用户手动操作
- 不再导出单独的 `camera.json`，改为写入总配置文件 **`dlssconfig.json`**

### 1.5 dlssconfig.json 格式

```jsonc
{
  "version": 1,
  "output_resolution": { "width": 3840, "height": 2160 },
  "render_resolution": { "width": 2227, "height": 1253 },
  "quality_mode": "Balanced",
  "display_device": "sRGB",
  "view_transform": "ACES 2.0",
  "is_hdr": false,
  "color_space": "aces_srgb",
  "fps": 30,
  "filename_pattern": "frame_\\d{4}\\.exr",
  "jitter_phases": 3,                  // base=1 × scale² (Blender 已 AA，base=1; ~1.72x²≈3)
  "frames": {
    "0001": {
      "matrix_world": [[...], ...],
      "projection": [[...], ...],
      "fov": 0.6911,
      "aspect_ratio": 1.7778,
      "near_clip": 0.1,
      "far_clip": 100.0,
      "jitter_x": -0.334,
      "jitter_y": 0.167
    }
    // ...
  }
}
```

**dlssconfig.json 包含 CLI 和 GUI 需要的全部参数**：最终分辨率、fps、DLSS 预设、颜色空间、文件名匹配模式、每帧相机+抖动数据。

---

## 二、DLSS CLI — 重构要点

### 2.1 通信协议

- **弃用 stdio 输出**，改用 **JSON-RPC**（参考 Language Server Protocol 模型）
- CLI 作为长期运行的后端进程，通过 stdin/stdout 或 TCP socket 提供 RPC 服务
- 备选方案：CLI 编译为 Flutter **Native Plugin**，走 MethodChannel（更干净，选型时评估）

### 2.2 渲染 API 选型

- 评估 **DX12** 重构，理由：
  - NGX SDK 的 DX12 路径更成熟（游戏引擎主流）
  - UE5 插件参考实现基于 D3D12
  - 但 DX12 需处理 swapchain/command list 等概念，Vulkan compute-only 当前够用
- **保持 Vulkan**，暂不迁移 DX12。若后续 DX12 有性能/稳定性优势再评估

### 2.3 管线架构 — 游戏管线风格

**输入资源**（从 Blender AOV EXR 获取）：
| 资源 | 来源 | 格式 | 分辨率 |
|------|------|------|--------|
| SceneColor | Combined.RGBA | R16G16B16A16_SFLOAT（场景线性 HDR） | 渲染分辨率 |
| Depth | Depth.Z | R32_SFLOAT（世界空间 → NDC [0,1]） | 渲染分辨率 |
| MotionVectors | Vector.XY | R16G16_SFLOAT（prev→curr → 取反为 curr→prev，像素空间） | 渲染分辨率 |

仅此三个资源。无 GBuffer（Albedo/Normals/Roughness），DLSS-SR 的 GBufferSurface 标记 "research only"（`nvsdk_ngx_helpers_vk.h:89-90`），不传。

**管线阶段**：

```
Stage 1: Frame Load + Preprocess                          [CPU + GPU]
├── AOV 异步预取：后台线程读下一帧 EXR
├── Depth 转换：world-space linear → NDC [0, 1]（Compute Shader）
├── MV 转换：Blender 4ch prev→curr → DLSS 2ch curr→prev（取反）
│     Sign: -(Blender.Vector.XY)  → DLSS 期望方向
│     Unit: 像素空间（render resolution）
│     MVScale: (1.0, 1.0)，MVLowRes=1，DLSS 内部缩放
└── 全部在渲染分辨率

         ↓  SceneColor + Depth + MV（渲染分辨率）+ Jitter（像素空间 [-0.5, +0.5]）

Stage 2: DLSS-SR Evaluate (if --scale > 1.0)              [GPU — NGX]
├── §3.1: SR 必须在 Tone Mapping 之前
├── pInColor        ← SceneColor（场景线性 HDR）
├── pInDepth        ← Depth（NDC [0,1]）
├── pInMotionVectors← MV（curr→prev, 像素空间）
├── InMVScale       = (1.0, 1.0)     // §3.6.3: 不需要缩放时为 1.0
├── InJitterOffset  ← dlssconfig.json per-frame jitter
├── InReset         ← 首帧 / gap / camera cut
├── InFrameTimeDelta= 1000/fps
├── IsHDR=true, AutoExposure=true     // §3.1.2: 线性空间必须 IsHDR=1
├── Debug overlay（dev DLL）           // ⚠️ 调研：离线无窗口环境是否可用，需验证
│     SR §4.2: dev DLL 含 overlay，release DLL 不含
└── pInOutput       → SceneColor（输出分辨率，场景线性）

         ↓  SceneColor（输出分辨率，场景线性 + 可选 debug overlay）

Stage 3: CAS Sharpening (可选, if --sharpness > 0)           [GPU — Compute]
├── DLSS 模型已训练产生清晰输出，默认不额外锐化
├── DLSS 内置 InSharpness 参数已废弃（§3.11）→ 不设置
├── CAS 为可选的额外空间锐化（compute shader，非 DLSS 内部）
├── 在 DLSS-SR 之后，输出分辨率，场景线性域
└── 默认 0.0（关闭），用户通过 CLI/GUI 显式开启

         ↓

Stage 4: Tone Mapping (if FG enabled)                     [GPU — Compute]
├── 管线输入始终为 scene-linear EXR（Blender EXR 不经 view transform）
├── 根据 dlssconfig.json 的 display_device 选择目标 EOTF：
│   ├── sRGB / Display P3 → sRGB EOTF → [0,1] display-ready (LDR)
│   └── Rec.2100 PQ → PQ(ST.2084) EOTF → [0,1] display-ready (HDR)
├── FG 需要 display-ready 输入
└── SR-only 路径：不经过此阶段

         ↓  backbuffer（输出分辨率，display-ready）+ Depth/MV（渲染分辨率）

Stage 5: DLSS-FG Evaluate (if FG enabled)                 [GPU — NGX]
├── FG §HDR: FG 操作 display-ready 颜色缓冲区
├── pBackbuffer     ← Stage 4 display-ready（sRGB 或 PQ-encoded [0,1]）
├── pDepth          ← Stage 1 Depth（渲染分辨率，复用）
├── pMVecs          ← Stage 1 MV（渲染分辨率，复用）
├── cameraViewToClip/prevClipToClip ← dlssconfig.json camera matrices
├── mvecScale       = (1/W, 1/H)    // 归一化到 [-1,1]
├── colorBuffersHDR = true
├── pOutputInterpFrame → display-ready 插值帧
└── 支持 2×/3×/4× multiFrame

         ↓  display-ready 插值帧（输出分辨率）

Stage 6: Inverse Tone Map (if FG enabled)                 [GPU — Compute]
├── 逆向 Stage 4 的 EOTF，恢复 scene-linear 用于 EXR 输出：
│   ├── sRGB EOTF → inv sRGB → scene-linear (LDR)
│   └── PQ EOTF → inv PQ → scene-linear (HDR)
├── 可用 --no-inverse-tonemap 跳过（输出保持 display-ready）
└── SR-only 路径：不经过此阶段

> 💡 **关于过去 "~10 nit clamp" 的问题**：Blender EXR 导出不经 view transform，FG 收到的是原始 scene-linear 数据。FG 文档要求输入为 display-ready（有 EOTF），scene-linear 直接传入导致 FG 内部处理异常，表现为亮度 clamp。我们在 FG 前自己加了 PQ EOTF，恰好修正了这个问题。不是 "FG 内部有 10-nit clamp"，而是 FG 在错误颜色空间下运行的结果。

Stage 7: Output Write (Async Queue)                       [CPU — I/O Thread]
├── 输出格式由 dlssconfig.json 的 is_hdr 字段决定可用选项：
│   HDR (is_hdr=true):                         LDR (is_hdr=false):
│   ├── EXR (R16G16B16A16_SFLOAT, scene-linear)├── EXR (R16G16B16A16_SFLOAT, sRGB encoded)
│   ├── ProRes 4444 XQ (12-bit, PQ encoded)    ├── PNG (16-bit)
│   ├── ProRes 422 HQ (10-bit, PQ encoded)     ├── JPEG (8-bit)
│   └── DNxHR HQX (12-bit, PQ encoded)         └── ProRes 422 HQ (LDR 兼容)
├── 编码参数（前端导出 Section 控制）：
│   ├── 格式选择（下拉框，根据 HDR/LDR 过滤选项）
│   ├── 压缩百分比（JPEG/PNG，滑块 0-100）
│   └── EXR 压缩算法（PIZ / ZIP / DWAA / DWAB）
├── FFmpeg 内置编译进 CLI（静态链接常用编解码器，不依赖系统 FFmpeg）
├── 异步写：EXR 写入或 FFmpeg 编码均不阻塞渲染线程
└── 前端导出 Section 提供格式选择 + 参数设置

                        ┌── SR-only: Stage 2 → (Sharpen) → 写输出（scene-linear EXR）
Pipeline 分支: ──────────┤
                        └── SR+FG: Stage 2 → (Sharpen) → 4(ToneMap) → 5(FG) → 6(InvToneMap) → 写输出
```

### 2.3.1 FG 集成要点（来自 FG 官方文档 §1, §5, §6）

**FG 管线位置**（FG §1 第 68-78 行管线图，精简去掉我们不做的 Post Effects / UI）：

```
Render (AOV) → [DLSS-SR] → [DLSS-FG]
     ↑                         ↑
 原始渲染帧                 最终输出帧
```

- SR 在 FG **之前**
- FG 处于管线**最末端**
- **SR 不在 FG 生成的帧上运行**——FG 在 SR 输出的帧之间插值

**FG Create 参数**（FG §5 第 459-464 行）：

```
Width/Height          = 输出分辨率（backbuffer 尺寸）
RenderWidth/Height    = 渲染分辨率（SR 输入分辨率）
NativeBackbufferFormat= 推荐 RGB10A2 for HDR (PQ)
```

**FG Eval 输入**（FG §6 第 531-549 行）：

| 输入 | 来源 | 分辨率 |
|------|------|--------|
| pBackbuffer | SR 输出 → tone-map → display-ready | 输出分辨率 |
| pDepth | AOV 原始深度（与给 SR 的是同一份） | 渲染分辨率 |
| pMVecs | AOV 原始 MV（与给 SR 的是同一份） | 渲染分辨率 |

> FG §6 原文："If an upscaler such as DLSS-SR is in use, this resource should be the same depth resource that was provided to the upscaler."

**FG 帧生成逻辑**（FG §1 第 86-92 行）：

```
Frame 1 (SR'd) ──→ FG interp 1.5 ──→ Frame 2 (SR'd) ──→ FG interp 2.5 ──→ Frame 3 (SR'd)
   Rendered          Generated          Rendered          Generated          Rendered
```

- FG 在每两个连续的 SR 输出帧之间生成插值帧
- 2x: 每对输入帧生成 1 帧 → 总输出 2x
- 4x: 每对输入帧生成 3 帧 → 总输出 4x
- FG 需要 current + previous backbuffer（pBackbuffer 自动保留上一帧）

**对我们的意义**：

```
SR-only:   AOV帧N → SR → 输出帧N
SR+FG:     AOV帧N → SR → ToneMap → FG(backbuffer_N, backbuffer_N-1) → 输出帧N, 插值帧N.5
           Depth/MV 直接复用给 FG（不经 SR 处理）
```

Depth 和 MV 在渲染分辨率，同时传给 SR 和 FG，两者各取所需。SR 把它们和 SceneColor 一起用；FG 把它们和 backbuffer 一起用。**无需为 FG 额外处理或重新上传。**

**与 DLSS 文档的对应关系**：

| 管线决策 | 文档依据 | 章节 |
|----------|----------|------|
| MV 格式：per-pixel, screen-space, curr→prev | "per-pixel motion vectors (describing the screen-space displacement of each pixel from the current to the previous frame)" | SR §3.6 |
| MV 格式：RG16_FLOAT 或 RG32_FLOAT | "Motion vectors: RG32_FLOAT or RG16_FLOAT" | SR §3.3 |
| SR 在 Tone Mapping 前 | "must occur before tone mapping" | SR §3.1 |
| SR 接受场景线性 HDR (IsHDR=1) | "linear space → IsHDR=1" | SR §3.1.2 |
| InMVScale = (1.0, 1.0) | "should be set to 1.0 if motion vectors do not need to be scaled" | SR §3.6.3 |
| MVLowRes = 1 | "MV at same resolution as input color" | SR §3.6.2 |
| FG 在 Tone Mapping 后 | "operates on final, display-ready color buffer" | FG §HDR |
| FG 在 SR 之后 | 管线图: Render → DLSS-SR → DLSS-FG（精简后，无 Post Effects） | FG §1 L68-78 |
| FG depth/MV 复用 SR 的 | "same depth resource that was provided to the upscaler" | FG §6 L544-545 |
| FG Create: Width=输出分辨率, RenderWidth=渲染分辨率 | "Width/Height: input/output color; RenderWidth/Height: pre-upscale" | FG §5 L459-464 |
| FG 生成帧在 SR 输出之间 | Frame sequence diagram: rendered → generated → rendered | FG §1 L86-92 |
| FG 输入需要 EOTF | "must already have tone mapping and required HDR EOTF applied" | FG §HDR |
| FG 不支持场景线性 | "scene-referred linear data are not supported" — 直接传入导致类似 clamp 的异常，需管线内 EOTF | FG §HDR |
| FG mvecScale 归一化 | "normalize to [-1,1] range" | FG SDK header |
| FG backbuffer 分辨率 | output resolution; depth/MV 为 render resolution | FG Create params |
| GBuffer 不传 | "OPTIONAL - only for research purposes" | `nvsdk_ngx_helpers_vk.h:89-90` |
| Debug overlay 绘于 output | "DLSS Debug Overlay" / "Jitter Debugging" / "Accumulation Mode" | SR §8.2-8.4 |
| Blender 色彩管理 | EXR 默认 scene-linear，view transform 仅影响显示预览和 PNG/JPEG 保存 | [Blender 5.1 Docs](https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html) |

### 2.4 性能优化

| 优化项           | 说明                                                  |
| ---------------- | ----------------------------------------------------- |
| **Texture 池化** | VkImage 跨帧复用，避免每帧 create/destroy             |
| **AOV 预取**     | 后台线程异步读取下一帧 EXR，GPU 处理不等待 IO         |
| **批量 Upload**  | 单 command buffer + 单 vkQueueSubmit 上传所有 texture |
| **异步写队列**   | EXR 写入或 FFmpeg 编码走独立线程，不阻塞渲染管线      |
| **FFmpeg 内置**  | 静态链接常用编解码器（ProRes/DNxHR），不依赖系统 PATH |
| **内存预算**     | `--memory-budget` 控制 GPU 纹理预取上限               |

### 2.5 RPC 接口设计

```
// 任务管理
dlss/setTask          { dlssconfig_path, preset_override?, fg_enabled?, fg_multiplier?,
                        output_format?,       // 格式由 HDR/LDR 限制可用选项
                        output_dir?,          // 输出目录
                        compression_quality?  // 0-100，仅 LDR JPEG/PNG
                        exr_compression? }    // "piz"|"zip"|"dwaa"|"dwab"
                      → { ok, task_id }
dlss/checkReady       {} → { ready: bool, reason?: string }
dlss/startTask        { task_id } → { ok }

// 状态查询
dlss/getStatus        {} → { dlss_version, fg_max_multiplier, gpu_name, driver_version,
                              is_hdr_supported,
                              video_encoders: ["prores_ks", "dnxhd"],   // FFmpeg 可用编码器
                              export_formats_hdr: ["exr", "prores_4444", "prores_422hq", "dnxhr_hqx"],
                              export_formats_ldr: ["exr", "png", "jpeg", "prores_422hq"] }
dlss/getTaskState     { task_id } → { state: "idle"|"running"|"done"|"error", current_frame, total_frames, eta_seconds }

// 执行状态（轮询用于进度条 + 帧预览）
dlss/getProgress      {} → { current_frame, total_frames, state }
dlss/getPreviewFrame  { frame_index } → { width, height, format, data: base64 }

// 调试（⚠️ 调研：离线无窗口环境下 dev DLL debug overlay 是否可用）
dlss/setDebugMode     { mode: "off"|"jitter"|"accumulation"|"exposure"|"mvec" }
                      → { ok }
dlss/isDebugDll       {} → { available: bool, version: string }

### 2.6 CLI 配置简化

由于大部分配置已在 Blender 导出的 `dlssconfig.json` 中确定（分辨率、HDR、fps、颜色空间），CLI 命令行参数大幅简化：

```
dlss-compositor.exe --config <dlssconfig.json>           # 从配置启动
                    --preset <J|K|L|M>                    # 覆盖预设
                    --fg <2x|4x>                           # 启用 FG
                    --listen <port>                        # RPC 监听模式 (0 = stdio)
                    --memory-budget <GB>                   # 内存预算
```

---

## 三、Flutter 前端 GUI

### 3.1 技术选型

- **框架**：Flutter（替代 Electron）
- **状态管理**：RxDart / BLoC（响应式、异步流）
- **数据持久化**：SharedPreferences
- **文件对话框**：win32 FFI（参考 `d:\FlutterProjects\shader_validation_utils\lib\utils\win_api.dart`）

### 3.2 通信方式

- **方案 A**：JSON-RPC over stdio socket（CLI 独立进程，前端通过 socket 通信）
- **方案 B**：Flutter Native Plugin（CLI 编译为 DLL，MethodChannel 直调 — 最干净）
- 选型时评估两种方案，倾向方案 B

所有调用**异步**，前端 UI 不阻塞等待 RPC 返回。

### 3.3 数据持久化

- 使用 SharedPreferences 保存：
  - 上次 `dlssconfig.json` 路径
  - 用户选择的 preset override
  - FG 倍率
  - 窗口布局状态（分隔栏位置）
- **会话标题**：默认 `{aov目录名}_{随机4位}`，允许用户自定义
- **Debug/Release 配置分离**：
  ```dart
  if (kDebugMode) {
    // 读取 debug 配置入口
    _prefs = await SharedPreferences.getInstance();
  } else {
    _prefs = await SharedPreferences.getInstance();
  }
  ```

### 3.4 布局结构

```
┌──────┬──────────────────────────────────────────────────┐
│      │ ┌──────────────────────┐ ┌─────────────────────┐ │
│      │ │                      │ │                     │ │
│ Menu │ │  任务配置容器         │ │  详情/预览容器       │ │
│ Bar  │ │  (dlssconfig 选择     │ │  (Quality Mode     │ │
│      │ │   + Preset 选择       │ │   + FG 选项        │ │
│ (竖) │ │   + FG 倍率           │ │   + 帧预览          │ │
│      │ │   + 开始按钮)         │ │   + 日志/统计)      │ │
│ ██   │ │                      │ │                     │ │
│ ██   │ └──────────────────────┘ └─────────────────────┘ │
│      │  ◄─────── 可拖拽分隔栏 ───────►                   │
│ ██   ├──────────────────────────────────────────────────┤
│      │ Status Bar: [DLSS v3.10.5 | FG Max 4x]  [█░░] 12/120 │
└──────┴──────────────────────────────────────────────────┘
```

菜单栏位于窗口**最左侧竖排**，类似 VS Code Activity Bar。右侧区域分为任务配置（左）和详情/预览（右）两个可拖拽容器。底部为状态栏。

### 3.5 左侧容器 — 任务配置

1. **dlssconfig 选择**
   - 文件选择按钮（调用 Win32 `FileOpenDialog`，仅选 `.json`）
   - 选中后显示文件路径

2. **加载后自动展示**（从 dlssconfig.json 读取）：
   - 输出分辨率：`3840 × 2160`
   - 渲染分辨率：`1920 × 1080`
   - Quality Mode：`Balanced` (1.72x)
   - 颜色空间：`Scene Linear / HDR`

3. **Preset 选择器**（下拉框）
   - 选项：Auto (默认) / J / K / L / M
   - 默认 Auto（按 DLSS 推荐的 per-quality 映射）

4. **FG 选项**
   - Toggle 开关（启用/禁用 FG）
   - FG 倍率：Toggle 按钮组 `[2x] [3x] [4x]`（最大倍率从后端查询）
   - 默认 2x

5. **输出配置 Section**
   - 输出格式选择（下拉框）
     - HDR 项目（`is_hdr=true`）：仅显示 HDR 兼容格式（EXR, ProRes 4444, ProRes 422 HQ, DNxHR HQX）
     - LDR 项目（`is_hdr=false`）：显示 EXR, PNG, JPEG, ProRes 422 HQ
     - 可用列表从 `dlss/getStatus` RPC 查询（`export_formats_hdr` / `export_formats_ldr`）
   - 压缩设置（条件显示）：
     - JPEG/PNG：滑块 0-100 压缩百分比
     - EXR：下拉框选择压缩算法（PIZ / ZIP / DWAA / DWAB）
     - ProRes/DNxHR：无压缩选项（编码器内置）
   - 输出目录选择（调用 Win32 文件夹对话框）
   - 默认值从 dlssconfig.json 继承，用户可覆盖

6. **开始按钮**
   - 点击后通过 RPC 设置任务 + 启动

### 3.6 右侧容器 — 详情 + 预览 + 调试

- 任务执行中显示实时帧预览（`dlss/getPreviewFrame` RPC）
- 预览帧含 DLSS debug overlay（如启用），直接呈现在前端
- **调试模式**（⚠️ 调研：离线无窗口环境下 dev DLL debug overlay 是否可用）
  - 若可行：通过 `dlss/isDebugDll` RPC 检测，dev DLL → 显示调试工具栏
  - 工具栏 Toggle：Jitter / Accumulation / Exposure / MV 可视化
  - 通过 `dlss/setDebugMode` RPC 切换，DLSS 直接绘制到 output buffer
  - 若不可行：降级为纯 shader 自绘 debug pass，或标记为 release DLL 不支持
  - 预览帧自动反映调试信息，无需额外渲染
- 导出进度：当前编码帧 / 总帧数
- 日志/错误信息
- 完成后显示统计（耗时、帧数、输出格式、文件大小）

### 3.7 状态栏

- **左侧**（固定信息，从 `dlss/getStatus` RPC 查询）：
  ```
  DLSS SDK 3.10.5 | GPU: RTX 4090 | FG Max: 4x
  ```
- **右侧**（动态信息，轮询 `dlss/getProgress`）：
  ```
  [████████░░] 24/120
  ```

### 3.8 RPC 轮询策略 (Rx)

```dart
// 每 500ms 轮询进度，节流 + 去重
Stream.periodic(Duration(milliseconds: 500))
  .asyncMap((_) => rpc.call('dlss/getProgress'))
  .distinct()                     // 去除连续相同状态
  .throttleTime(Duration(ms: 500)) // 节流
  .listen((progress) {
    setState(() => _progress = progress);
  });
```

---

## 四、Win32 文件对话框参考实现

（来自 `d:\FlutterProjects\shader_validation_utils\lib\utils\win_api.dart`，用于 Flutter 前端打开 `dlssconfig.json`）

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:win32/win32.dart';

String selectFile() {
  var hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (!SUCCEEDED(hr)) return "";

  final fileDialog = FileOpenDialog.createInstance();
  final pfos = calloc<Uint32>();
  hr = fileDialog.getOptions(pfos);
  final options = pfos.value | FOS_FORCEFILESYSTEM; // FOS_PICKFOLDERS 用于选目录
  fileDialog.setOptions(options);

  // JSON 文件过滤
  final rgSpec = calloc<COMDLG_FILTERSPEC>(1);
  rgSpec[0]
    ..pszName = TEXT('JSON Files')
    ..pszSpec = TEXT('*.json');
  fileDialog.setFileTypes(1, rgSpec);

  hr = fileDialog.show(NULL);
  if (!SUCCEEDED(hr)) return "";

  final ppsi = calloc<COMObject>();
  fileDialog.getResult(ppsi.cast());
  final item = IShellItem(ppsi);
  final pathPtr = calloc<Pointer<Utf16>>();
  item.getDisplayName(SIGDN_FILESYSPATH, pathPtr);
  return pathPtr.value.toDartString();
}
```

---

## 五、与当前实现的差异总结

| 方面               | 当前 (v1)               | 目标 (v2)                                         |
| ------------------ | ----------------------- | ------------------------------------------------- |
| **Blender 输出**   | 手动导出 camera.json    | **dlssconfig.json** 自动静默导出                  |
| **分辨率控制**     | CLI `--scale` 参数      | **Blender 面板** 控制最终分辨率 + quality mode    |
| **CLI 通信**       | stdio 文本输出 + NDJSON | **JSON-RPC**（LSP 模型）                          |
| **渲染 API**       | Vulkan compute-only     | Vulkan（保留），可选评估 DX12                     |
| **管线风格**       | 离线批量处理器          | **游戏管线风格**（GBuffer → DLSS → Tonemap → FG） |
| **性能**           | 单纹理逐帧 upload       | Texture 池化 + 批量 upload + 异步预取 + 写队列    |
| **前端**           | Electron + React        | **Flutter**                                       |
| **状态管理**       | useReducer              | RxDart / BLoC                                     |
| **配置持久化**     | JSON 文件               | SharedPreferences                                 |
| **InMVScale**      | (2.0, 2.0) — **bug**    | **(1.0, 1.0)** ✅                                  |
| **pInOutput 清零** | 每帧清零                | 仅首帧/reset                                      |
| **README**         | 虚假 "10-nit clamp"     | 更正为 FG §HDR 真实要求                           |
| **GBuffer for SR** | 已删除 (Phase 2)        | 确认不恢复                                        |

---

## 六、用户原话

ok我看了下汇总的内容, 我们来整理一下后面重构的大致目标.
首先整套流程由三个部分组成, blender配置插件, 前端gui, dlss cli, 整体上还是这样就行.
blender插件这块, 由于我们现在知道jitter phase和缩放比率相关, 那么分辨率相关的设置最好由blender插件这边控制, 为了方便设置, 插件面板需要提供最终分辨率, 然后给一个横排toggle, 只能单选quality mode, 从ultra quality到dlaa, 按照dlss给的预设来排. 我们设置到blender的渲染分辨率和输出比例上. 依然我们要给一个配置合成器和pass的按钮. 导出的相机数据这块我们做成静默的, 直接用handler的形式静默执行, 我们需要写入一个总配置(比如叫dlssconfig.json), 包含最终分辨率, fps, 选择的dlss预设, 颜色空间, 以及文件名的匹配方式等后续需要的参数.
dlss cli这块, 我们这次不适用stdio的输出, 参考`Language Server Protocol`, 用json rpc应该比较方便, 通过连接后端然后查询来拿到各种前端需要的数据, 已经后端执行任务的数据. 我们需要评估是否要用dx12重构. 重构后的管线应该类似游戏管线, 以方便dlss插入正确的位置. gbuffer生产通过读取blender生成的aov, 然后插入dlss, 然后是tonemapping, 当然这块需要重新阅读dlss实现文档小心重构. 新的管线同时要优化效率, texture池化, aov加载预读, 写输出队列化不卡住渲染线程. cli接受的输入这块到时候按照dlss实现文档重新设计, 由于很多东西在blender输出的时候就决定了, 比如是否hdr, 于是乎我们需要给的接口就比较少了, 设置dlss任务的参数分开接口, 需要一个接口检查是否准备就绪, 需要接口查询dlss状态, 需要接口查询当前dlss执行状态(当前处理的第几帧, 返回二进制图像数组用于前端预览等).
前端这次我们换成flutter, 主题设计上之后再说这里不讨论了, 前端需要数据持久化, 保留用户配置, 允许用户保存上次会话的配置, aov路径, 会话标题(默认给aov路径+随机数或者guid)允许自定义, 由于调试也用的同一个前端, flutter可以用if debug区分, 在debug的时候另存一份配置入口. sharedpereference应该能做到. 前端通过类似lsp查询的方式与后端通信, 所有调用异步, DSL模式或者rx之类的框架应该很适合, 前端可能是个带状态栏的左右结构的布局, 允许用户调整左右容器大小. 菜单栏看情况到时候有多少page吧, 聚焦于任务页的话, 需要用户指定blender导出的dlssconfig, 打开文件参考这个原生一些的实现`d:\FlutterProjects\shader_validation_utils\lib\utils\win_api.dart`, 一会儿生成的计划里贴上实现细节作为后续参考, 我们所有数据从这里拿, 右边详情容器里展示quality mode等详细信息, dlssconfig下面用户可以选择preset, 下拉框的形式, 然后还有fg的选项, fg倍率, 默认2x, 用toggle的形式展示好了, 整数倍, 最大倍率从后端拿. 状态栏需要展示dlss后端的一些信息, 目前的gui有dlss版本, 和fg最大倍率. 状态栏分为左右两边好了, 左边是版本dlss相关的, 右边对齐的部分展示一个比较短的进度条(任务开始后), 一个x/xx的当前处理的帧数量. 数据都从后端查询来拿, 条件上参考rx的话就是repeat+throttle+上一次请求完成后. 开始任务以及设置任务都通过rpc.
当然还有个方案是cli作为flutter的一个native plugin, 走methodchannel, 这应该是最干净的. 交给之后选型再设计了.

---

## 七、待决定事项

- [ ] CLI 通信最终选型：JSON-RPC stdio socket vs Flutter Native Plugin (MethodChannel)
- [ ] DX12 迁移是否值得（Vulkan compute-only 当前够用，但 DX12 NGX 路径更主流）
- [ ] GBuffer 生产：从 AOV 通道重建（Albedo/Normals/Roughness）是否恢复？（当前 Phase 2 已删，文档标记 "research only"）
- [ ] FG backbuffer 格式：保持 R16G16B16A16_SFLOAT 还是改为 FG 推荐的 RGB10A2
- [ ] **视频输出格式**：ProRes 4444 XQ vs 422 HQ vs DNxHR — 需调研各格式的 HDR 支持、色彩精度、文件大小、编码性能、跨平台兼容性
