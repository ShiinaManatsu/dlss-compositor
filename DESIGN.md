# DLSS Compositor v3 — Architecture Design

> 状态：草案 / 待审阅  
> 作者：Claude  
> 基于：V3 分支、d3d12-rr 实验、RR 集成文档、310.7.129 DLL  

---

## 0. 背景与目标

**目标**：提供一套工具，让 Blender 视频组用 DLSS（SR/RR→FG）加速渲染流程。

**核心变化（v2→v3）**：

| 维度 | v2 | v3 |
|---|---|---|
| 前端 | Flutter Windows | Electron + React (Adobe Spectrum) |
| 后端 | Flutter native plugin (C++/Vulkan) | Electron IPC → Python 编排 → C++/Vulkan CLI |
| 管线触发 | GUI 发起任务→CLI | Electron 发起→Python 注入 Blender→CLI |
| RR 支持 | 无 | Cycles 用 RR→FG，EEVEE 用 SR→FG |
| 项目格式 | 无 | `.dcp`（DLSS Compositor Project，JSON） |
| DLL 来源 | submodule libs | 从 dlss-swapper 下载 310.7.129.0 |

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────┐
│  Electron + React (Adobe Spectrum)                  │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────┐ │
│  │ 项目管理     │ │ 设置 + 预览  │ │ Job 列表    │ │
│  └──────────────┘ └──────────────┘ └─────────────┘ │
│        │ IPC (JSON + ArrayBuffer for preview)       │
└────────┼────────────────────────────────────────────┘
         │
┌────────▼───────────────────────────────────────────┐
│  Electron Main Process（orchestrator.ts）          │
│  · 管理子进程生命周期                               │
│  · 转发 IPC ↔ 后端 stdio/named-pipe                │
└────────┬───────────────────────────────────────────┘
         │ spawn  
┌────────▼───────────────────────────────────────────┐
│  Python Orchestrator（pipeline/run.py）            │
│  · 生命周期：内存盘创建→注入→渲染→DLSS→输出→卸载  │
│  · stdout 输出 DLSS_EVENT: JSON 行给 Electron      │
│  · 管理 blender.exe + dlss-compositor-cli 子进程   │
└────┬──────────────┬────────────────────────────────┘
     │              │
     │ spawn        │ spawn + JSON-RPC (stdio)
     │              │
┌────▼────┐   ┌─────▼──────────────────────────────┐
│ blender │   │  dlss-compositor-cli (C++/Vulkan)   │
│ -b      │   │  · NgxContext（SR + RR + FG）       │
│ inject. │   │  · SrFgPipeline / RrFgPipeline      │
│ py      │   │  · FileWatcher（folder watch）      │
│         │   │  · OIIO/FFmpeg 输出                 │
└─────────┘   └────────────────────────────────────┘
```

---

## 2. DLL 管理策略

submodule (`DLSS/`) 只用于：
- **头文件**（include/）
- **导入库**（lib/）
- **CMake 集成**

运行时 DLL **不打包进 repo**，通过以下方式获取：

```
tools/
  dlss_updater.py      # 从 dlss-swapper manifest 下载最新 DLL
  dlss_versions.json   # 锁定版本信息
resources/
  nvngx_dlss.dll       # SR  310.7.129.0  (gitignored, auto-download)
  nvngx_dlssd.dll      # RR  310.7.129.0
  nvngx_dlssg.dll      # FG  310.7.129.0
```

`dlss_versions.json` 格式（锁定，类似 lockfile）：
```json
{
  "sr":  { "version": "310.7.129.0", "md5": "D54BE95CFD9DF08C6EBA49DA2E1A2B7E",
           "url": "https://dlss-swapper-downloads.beeradmoore.com/dlss/dlss_v310.7.129.0_D54BE95CFD9DF08C6EBA49DA2E1A2B7E.zip" },
  "rr":  { "version": "310.7.129.0", "md5": "C02E1078AB74415BDA338283D08B1160",
           "url": "https://dlss-swapper-downloads.beeradmoore.com/dlss_d/dlss_d_v310.7.129.0_C02E1078AB74415BDA338283D08B1160.zip" },
  "fg":  { "version": "310.7.129.0", "md5": "FCE136870CEC8279A6598367E4EA0AF2",
           "url": "https://dlss-swapper-downloads.beeradmoore.com/dlss_g/dlss_g_v310.7.129.0_FCE136870CEC8279A6598367E4EA0AF2.zip" }
}
```

首次运行 / 版本不符时，Electron 启动前调用 `dlss_updater.py --check` 自动下载。

---

## 3. 项目格式 `.dcp`

DCP = DLSS Compositor Project，UTF-8 JSON 文件。

```json
{
  "version": 1,
  "name": "TypeAsuka SnowMix",
  "blend": "F:/Projects/Blender/Films/TypeAsuka/SnowMix/5.render_material_remaster.blend",

  "blend_overrides": {
    "enabled": false,
    "engine": null,
    "samples": null,
    "use_adaptive_sampling": null,
    "adaptive_threshold": null,
    "use_denoising": false,
    "denoiser": null,
    "noise_type": "white"
  },

  "dlss": {
    "mode": "auto",
    "sr_preset": "K",
    "rr_preset": "E",
    "quality": "QUALITY",
    "fg_multiplier": 2,
    "native_res_buffers": false,
    "ram_disk_gb": 8
  },

  "export": {
    "frame_start": 300,
    "frame_end": 350,
    "output_dir": "F:/Export/TypeAsuka/SnowMix",
    "filename_template": "{Name}_{Version}_{Frame}",
    "format": "prores_4444",
    "width": 3840,
    "height": 2160
  },

  "advanced": {
    "blender_exe": null,
    "ram_disk_letter": "R",
    "ram_disk_size_gb": 8
  }
}
```

`blend_overrides.enabled` = false 时，所有 override 字段忽略，Blender 按原始设置渲染。

`dlss.mode`：
- `"auto"` — 读取 blend 引擎自动判断（CYCLES→RR，EEVEE→SR）
- `"sr"` — 强制 SR
- `"rr"` — 强制 RR（需要 CYCLES 引擎）

---

## 4. 管线详细设计

### 4.1 整体流程

```
开始任务
  │
  ├─ [内存盘] 确认 R: 已挂载（用户预先创建或自动 imdisk）
  │
  ├─ [inspect] blender -b blend.blend --python inspect_blend.py
  │     输出：引擎、分辨率、帧范围、Cycles 参数 → session_state.json
  │
  ├─ [config] 生成 dlssconfig.json（jitter 表、camera matrix、LUT）
  │     blender -b blend.blend --python inject.py -- --dry-run ...
  │
  ├─ [render 阶段 1]（如果 SR + native_res_buffers 开启）
  │     blender -b ... --python inject.py -- --pass guides --percentage 100 --skip-config
  │     → R:\guides\####.exr（含满分辨率 Z + Vector）
  │
  ├─ [render 阶段 2]（主渲染）
  │     blender -b ... --python inject.py -- --mode sr/rr --percentage N ...
  │     → R:\color\####.exr（SR 模式）
  │       或 R:\combined\####.exr（RR 模式，含所有 guide buffer）
  │
  ├─ [DLSS 循环]（dlss-compositor-cli 常驻，JSON-RPC）
  │     FileWatcher 监听 R:\color\ 或 R:\combined\
  │     每帧：读 EXR → 解包 buffers → SR/RR eval → FG eval → 写输出
  │
  └─ [输出]（OIIO / FFmpeg）
        ProRes 4444 MOV / EXR seq / PNG seq
```

### 4.2 SR 双趟（native_res_buffers）

当 SR 且质量 ≠ DLAA 且用户选择高精度 buffer 时：

```
趟 1：--pass guides --percentage 100
      输出：R:\guides\####.exr（Vector 4ch + Depth）
趟 2：--pass beauty --percentage N（正常渲染百分比）
      输出：R:\color\####.exr（Combined half-float）

DLSS 循环：
  读 color/####.exr → SR 的 pInColor
  读 guides/####.exr → SR 的 pInDepth, pInMotionVectors
  → SR eval（render-res color + output-res depth/MV）
```

内存盘容量规划：
- beauty 趟最大帧 ≈ `render_w × render_h × 8 bytes`（half-float RGBA）
- guides 趟最大帧 ≈ `output_w × output_h × 12 bytes`（Z float32 + Vector half 4ch）
- 窗口大小 = `ram_disk_bytes × fill_factor / max_frame_size`（V3 `plan_ram_windows` 已实现）

### 4.3 RR 管线

RR EXR 包含的 channels（Blender 5.3+ Cycles）：

| 用途 | Blender pass | EXR channel | RR 参数 |
|---|---|---|---|
| 噪声颜色 | Image (Combined) | `.R/.G/.B` | `pInColor` |
| 深度 | Denoising Depth | `Denoising Depth.V` | `pInDepth`（线性深度） |
| 运动向量 | Vector | `Vector.X/.Y` | `pInMotionVectors` |
| 漫反射反照率 | Denoising Albedo | `.R/.G/.B` | `pInDiffuseAlbedo` |
| 高光反照率 | Denoising Specular Albedo | `.R/.G/.B` | `pInSpecularAlbedo` |
| 法线 | Denoising Normal | `.X/.Y/.Z` | `pInNormals` |
| 粗糙度 | Denoising Roughness | `.V` | `pInRoughness` |
| 镜面运动 | Denoising Specular Motion | `.X/.Y` | `pInMotionVectorsReflection` |
| 反向运动（可选） | Denoising Backward Motion | `.X/.Y` | — |

> **注意**：SDK 头文件里 RR Preset F 标注"Do not use, reverts to default"，但实验用的 310.7.128 DLL 中 F 实为 RR2（需要驱动 ≥ 580）。310.7.129 DLL 来源同一系列。UI 里暂时提供 D/E 选项，F 加 beta 标签，测试确认后去掉标签。

RR 关于 noise type（摘自 RR 文档 §3.5）：
- 推荐：白噪音 / 高质量 hash（xxhash32 等）
- 蓝噪音：条件允许（period 足够大、无相关性时可用）
- Cycles 默认蓝噪音，文档建议换白噪音（sobol 或 tabulated）
- `blend_overrides.noise_type = "white"` 时，注入脚本通过 `scene.cycles.sampling_pattern` 切换

### 4.4 FG 管线（RR 之后）

RR 输出是 display-ready（经过 color management）→ 可以直接进 FG，与 SR 后相同。

FG 输入：
- 当前帧输出（RR/SR 输出）
- 上一帧输出（FG 内部 buffer）
- 深度（与 SR/RR 共享的同一 pool handle）
- 运动向量（同上）
- Camera matrix（来自 dlssconfig）

---

## 5. 后端模块

### 5.1 Python Orchestrator（pipeline/）

复用 V3 分支的以下模块（基本稳定）：
- `inject.py` — Blender 注入（支持 sr/rr/guides pass）
- `inspect_blend.py` / `inspect_scene.py` — 读取 blend 元数据
- `jitter.py` — jitter 相位表
- `ramdisk.py` — ImDisk 管理
- `session_config.py` — 窗口规划、配置验证

**新增/修改**：
- `session_config.py`：增加 RR noise_type override
- `inject.py`：增加 `Denoising Specular Motion` pass 的 socket 连接（Blender 5.3）
- `run.py`：根据 `.dcp` 配置决定管线模式，添加 RR 路径

### 5.2 C++ CLI（dlss-plugin/）

**新增 RrFgPipeline**，结构参考 `SrFgPipeline`：

```cpp
// dlss-plugin/src/pipeline/rr_fg_pipeline.h
class RrFgPipeline {
    void run(const RrFgConfig& config);
};

struct RrFgConfig {
    DlssConfig dlssConfig;
    // 与 SrFgConfig 相同的 outputFormat/dir/template 等
    // 额外：
    RrPreset rrPreset = RrPreset::E;       // 默认 E
    RrRoughnessMode roughnessMode = RrRoughnessMode::Unpacked;
    bool hasSpecularMotion = false;        // Blender 5.3+ 时为 true
};
```

**新增 DlssRrPass**（参考 d3d12-rr 实验的 Vulkan 版本）：

RR Vulkan API：使用 `nvsdk_ngx_helpers_dlssd_vk.h`（DLSS SDK 已有），EvalParams 与 d3d12-rr 的 D3D12 结构对应。

**NgxContext 扩展**：
```cpp
bool isRrAvailable() const { return m_rrAvailable; }
NgxDllVersions getDllVersions() const; // 增加 rr 字段
```

### 5.3 文件监听（FileWatcher）

V2 已有 `FramePrefetcher`（预读），v3 在此基础上：
- 监听 `R:\combined\` 目录
- 新文件写完（文件大小稳定 100ms）后触发
- 读取并路由到对应 pipeline
- 消费后删除 EXR（释放内存盘空间）

### 5.4 输出格式

保持 v2 已实现的格式：
- EXR scene-linear（直接来自 SR/RR，不经 tonemap）
- PNG16
- ProRes 422 HQ（通过 `ProResWriter`）
- **新增**：ProRes 4444（支持 alpha）

---

## 6. 前端

### 6.1 技术栈

```
Electron 33+
├── Main: TypeScript（复用 V3 orchestrator.ts 逻辑）
├── Renderer: React 18 + Vite
├── UI: @adobe/react-spectrum + Spectrum tokens
└── 通信: contextBridge IPC（JSON + ArrayBuffer for preview）
```

Adobe Spectrum 选型理由：符合"专业简洁"定位，内置 dark/light theme，组件库完善（Picker、NumberField、Switch、ProgressBar、ListView 都有）。

### 6.2 屏幕设计

**启动屏：项目选择**
```
┌─────────────────────────────────────────────────┐
│  DLSS Compositor                        [─][□][×]│
├─────────────────────────────────────────────────┤
│  最近项目                                        │
│  ┌──────────────────────────────────────────┐   │
│  │ 📄 TypeAsuka SnowMix          2025-08-26 │   │
│  │ 📄 Project B                  2025-08-10 │   │
│  └──────────────────────────────────────────┘   │
│                                                  │
│  [+ 新建项目]  [📂 打开 .dcp]  [🗑 删除]        │
└─────────────────────────────────────────────────┘
```

**主工作区（项目已加载）**

三栏布局：

```
┌──────────┬───────────────────────────────┬──────────────────┐
│ 设置面板 │  预览                         │ Job 列表         │
│          │  [当前帧预览 / 进度动画]       │                  │
│ ▸ 文件   │                               │ ● 任务 1 运行中  │
│ ▸ DLSS   │                               │ ✓ 任务 2 完成    │
│ ▸ 导出   │  Frame 325 / 350              │   [打开文件夹]   │
│ ▸ 高级   │  [RR→FG]  [1920×1080→3840]   │                  │
│          │                               │                  │
│          ├───────────────────────────────┤                  │
│          │ 进度条  ████████░░  78%       │                  │
│          │ 预计剩余 4m 32s               │                  │
│          │ [▶ 发起任务]  [⏹ 停止]        │                  │
└──────────┴───────────────────────────────┴──────────────────┘
```

**设置面板详细（各折叠组）**

*▸ Blend 文件*
- blend 文件路径（只读 + [更改]按钮）
- 引擎：EEVEE-NEXT / Cycles（从 blend 读取，只读）
- 帧范围（从 blend 读取，可 override）
- 渲染分辨率（从 blend 读取）
- Override 开关（打开后以下项变可编辑）
  - 最大采样次数
  - 降噪（勾选/取消）
  - 降噪器（OIDN / OptiX）
  - 噪音类型（白噪音 / 蓝噪音）—— RR 推荐白噪音

*▸ DLSS*
- 模式：自动 / SR / RR（依据引擎自动判断，可手动覆盖）
- 质量：DLAA / Ultra Quality / Quality / Balanced / Performance / Ultra Performance
- SR 时：
  - SR Preset（M 默认 Perf 档 / K 默认 Quality 档）
  - 高精度 Buffer（native_res_buffers，适合大幅度 SR 时提升质量）
- RR 时：
  - RR Preset（E 默认 / D / F[beta]）
  - 粗糙度模式（Unpacked / Packed，通常保持 Unpacked）
- FG 倍率（2× / 3× / 4×）—— 上限由 NGX 能力查询决定

*▸ 导出*
- 导出分辨率（宽/高，默认读 blend 全分辨率）
- 格式：ProRes 4444 MOV / ProRes 422 HQ MOV / EXR Sequence / PNG16 Sequence
- 输出目录
- 文件名模板（支持 `{Name}` `{Version}` `{Frame}` `{Date}`）
- 帧范围覆盖（可单独设置，默认同 blend）

*▸ 高级*
- Blender 路径（留空自动检测）
- 内存盘盘符（默认 R）
- 内存盘大小 GB（默认 8）

### 6.3 预览传输

CLI 输出帧时，通过 JSON-RPC callback 传 JPEG preview（512px 宽，80%质量）：

```
CLI --[RPC]--> Python run.py --[stdout DLSS_EVENT:]--> Electron main --[IPC]--> renderer
```

preview 以 `ArrayBuffer` / `Uint8Array` 传输，renderer 用 `URL.createObjectURL` 显示。

### 6.4 Job 列表

每个 job 条目：
```typescript
type Job = {
  id: string;
  name: string;          // "{blend basename} {timestamp}"
  status: 'running' | 'done' | 'error' | 'cancelled';
  mode: 'sr' | 'rr';
  frameStart: number;
  frameEnd: number;
  frameDone: number;
  outputDir: string;
  createdAt: number;
  error?: string;
};
```

点击"打开文件夹"调用 `shell.openPath(job.outputDir)`。新 job 插入顶部。

---

## 7. 进程通信协议

### 7.1 Python→Electron（stdout 事件流，复用 V3）

```
DLSS_EVENT:{"event":"inject_ready","mode":"rr","fps":24,...}
DLSS_EVENT:{"event":"progress","frame":325,"total":350,...}
DLSS_EVENT:{"event":"frame_done","frame":325,...}
DLSS_EVENT:{"event":"preview","data":"base64jpeg..."} 
DLSS_EVENT:{"event":"complete","output_dir":"..."}
DLSS_EVENT:{"event":"error","message":"..."}
```

### 7.2 Electron→Python（stdin 命令）

```json
{"cmd": "stop"}
{"cmd": "ping"}
```

### 7.3 Python→CLI（JSON-RPC stdio，复用现有 RpcDispatcher）

```json
{"jsonrpc":"2.0","method":"startTask","id":1,"params":{...}}
{"jsonrpc":"2.0","method":"getStatus","id":2}
{"jsonrpc":"2.0","method":"stopTask","id":3}
```

---

## 8. 构建布局

```
dlss-compositor/
├── DLSS/                      # submodule（头文件 + 导入库）
├── dlss-plugin/               # C++ Vulkan pipeline（改名 backend/）
│   └── src/
│       ├── dlss/              # SR pass（现有）
│       │   ├── dlss_rr.cpp/h  # 新增：RR pass
│       │   └── ...
│       ├── pipeline/
│       │   ├── sr_fg_pipeline # 现有
│       │   └── rr_fg_pipeline # 新增
│       └── ...
├── pipeline/                  # Python orchestrator（复用 V3）
├── app/                       # Electron + React 前端（V3 架构）
│   ├── electron/
│   │   ├── main.ts
│   │   ├── orchestrator.ts    # 复用 V3
│   │   └── ...
│   └── src/                   # React UI
│       ├── App.tsx
│       ├── screens/
│       │   ├── HomeScreen.tsx
│       │   └── ProjectScreen.tsx
│       ├── components/
│       │   ├── SettingsPanel.tsx
│       │   ├── PreviewPane.tsx
│       │   └── JobList.tsx
│       └── ...
├── tools/
│   ├── dlss_updater.py        # 下载 310.7.129 DLL
│   └── dlss_versions.json     # 版本锁定
└── resources/                 # DLL（gitignored）
    ├── nvngx_dlss.dll
    ├── nvngx_dlssd.dll
    └── nvngx_dlssg.dll
```

---

## 9. 实施顺序

### Phase 1：后端跑通

1. **RR pass（C++）**：基于 d3d12-rr 实验，移植到 Vulkan（参考 `dlss-plugin/src/dlss/dlss_sr.cpp`）
   - `DlssRrPass`：封装 `NGX_DLSSD_CREATE_VK_EXT` + `NGX_DLSSD_EVALUATE_VK_EXT`
   - `NgxContext` 扩展 `isRrAvailable()`、`rr` DLL 版本字段
2. **RR EXR 读取**：基于 d3d12-rr `rr_exr.cpp` 移植到 Vulkan pipeline，加 Specular Motion 支持
3. **RrFgPipeline**：参照 `SrFgPipeline`，输入多 4 个 guide buffer
4. **inject.py 更新**：增加 `Denoising Specular Motion` pass socket（Blender 5.3）
5. **测试**：使用 `5.render_material_remaster.blend`，帧 300-350

### Phase 2：Electron + React UI

6. **DCP 读写**：`project.ts` 扩展，支持新字段
7. **设置面板**：Adobe Spectrum 组件
8. **Job 列表 + 进度**：复用 V3 progress.ts 逻辑
9. **预览**：preview 事件→ArrayBuffer→img src

### Phase 3：打包与发布

10. **DLL 下载器**：`tools/dlss_updater.py`
11. **发布脚本**：更新 `scripts/package_release.ps1`

---

## 10. 已知问题 / 待确认

| 编号 | 问题 | 结论 |
|---|---|---|
| RR-P1 | RR Preset F 在 310.7.129 DLL 中是 RR2 还是 default？ | 实验 310.7.128 确认是 RR2（需驱动≥580）；310.7.129 待测，UI 先加 beta 标签 |
| RR-P2 | Blender 5.3 Denoising Specular Motion socket 名称 | 屏幕截图可见 "Denoising Specular Motion"，inject.py 需验证 `rl.outputs.get("Denoising Specular Motion")` |
| SR-P1 | native_res_buffers 双趟内存盘空间规划 | V3 `plan_ram_windows` 已实现，需适配双趟场景 |
| FG-P1 | RR→FG 与 SR→FG 的区别 | 管线层面相同；RR 输出已是 display-ready，进 FG 前不需要额外 tonemap |
| DLL-P1 | 310.7.129 DLL 下载：Electron 首次运行自动触发 | 需要确认网络/签名验证策略 |
