# DLSS Compositor v3 — Architecture Design

> 状态：草案 / 待审阅（§6 前端已按 Spectrum 2 rail 结构落地）  
> 作者：Claude  
> 基于：V3 分支、d3d12-rr 实验、RR 集成文档、310.7.129 DLL  
>
> **2026-08-28（晚）**：前端已用 Spectrum 2 重新设计落地（`app/src`）：左侧 rail 导航（项目/导出/Jobs/设置），面包屑标题栏，导出页 = 左设置面板 + 右预览叠进度条，标题栏右上发起任务/停止；Jobs 页列产出（序列/视频各一条，随 `.dcp` 持久化 `history`）。旧 V3 UI 保留在 `app/ref-v3ui/`（不参与编译）。  
> **2026-08-28**：Phase 1 后端主路径已实测通过；预览 JPEG（回读→JPEG→RPC→base64）已落地。  

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

运行时 DLL **不进 git**；按 DLSS 文档（Programming Guide §5.1），**feature DLL 随应用分发，放在 exe 旁**（NGX core `_nvngx.dll` 由驱动提供，不打包）。

- **Debug**：CMake POST_BUILD 把 `resources/` 里锁死的 310.7.129.0 拷到 exe 旁（开发可复现）。
- **Release**：发布 zip 同样携带三份 `nvngx_*.dll`（exe 旁）；NGX Init 通过 NGX core 定位它们，无需驱动内搜索。
- `tools/dlss_versions.json` 只给开发锁版本用，**不是**发布下载器。

开发锁版本仍可从 dlss-swapper 取（不进发布包）：

```
tools/
  dlss_versions.json   # 开发锁版本（310.7.129.0 + md5 + url）
resources/             # gitignored；仅 Debug POST_BUILD 拷到 exe 旁
  nvngx_dlss.dll
  nvngx_dlssd.dll
  nvngx_dlssg.dll
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

Debug 构建拷贝 `resources/`；Release zip 携带同样三份 DLL（exe 旁），都不下载。

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

### 5.2 C++ CLI（cli/）

**新增 RrFgPipeline**，结构参考 `SrFgPipeline`：

```cpp
// cli/src/pipeline/rr_fg_pipeline.h
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

## 6. 前端（已按 Spectrum 2 落地）

### 6.1 技术栈

```
Electron 36
├── Main: TypeScript（electron/orchestrator.ts ↔ 子进程，stdout DLSS_EVENT 转发）
├── Renderer: React 19 + Vite 7
├── UI: @react-spectrum/s2（Spectrum 2，style macro + tokens）
└── 通信: contextBridge（window.dlss.*，JSON + base64 JPEG 预览）
```

选型理由：符合"专业简洁"定位，内置 dark/light theme，控件齐全（Picker、NumberField、Switch、ProgressBar、Disclosure、ListView、ActionMenu、CardView、Tabs）。

### 6.2 屏幕结构（rail 四页）

左侧竖排导航（S2 Tabs vertical：项目/导出/Jobs/设置），标题栏 = logo + 面包屑（`项目名 > 页面`）+ 窗口控制；导出页时标题栏右上为 **发起任务**（运行中变 **停止**）。

- **项目**（首屏）：最近 `.dcp` 卡片网格（CardView/AssetCard），右上 [打开][新建项目]。
- **导出**：左侧设置面板，四个 Disclosure 组 ——
  - ▸ Blend 文件：blend 路径+[读取]、引擎/分辨率/降噪 meta（只读）、起始/结束帧与采样/降噪 override（pin 覆盖）
  - ▸ DLSS：模式（SR/RR）、质量档 + 推荐百分比、渲染百分比（RR 隐藏并提示质量档）、原生分辨率缓冲（SR+Cycles）、帧生成（关/2x–6x）
  - ▸ 导出：宽/高、编码（ProRes 422 HQ / EXR / PNG16）、文件名模板 + 版本、目录
  - ▸ 高级：消费窗口、ImDisk/内存盘状态
  右侧预览：JPEG 预览（512px readback → base64 → objectURL），**底部叠加进度条**（按 Blender 帧数计百分比 + 第 x/y 帧 + 每/帧 + 剩余时间）。
- **Jobs**：当前项目产出列表（ListView），序列与视频各一条（模板前缀+帧区间 / 单文件），`…` 菜单：打开文件夹 / 打开文件 / 复制路径；序列帧数由 `fs:listDir` 实扫。记录随 `.dcp` 的 `history` 字段持久化。
- **设置**：颜色方案（深/浅/跟随系统）、环境探测（Blender/ImDisk/内存盘）、DLSS DLL 版本（锁定 310.7.129.0）、关于。

状态集中在 `src/appState.tsx`（context）：全部设置、事件流（inspect/progress/preview/export_done→history）、自动保存 .dcp。页面组件在 `src/pages/`。旧 V3 界面留在 `app/ref-v3ui/` 供参考，不参与编译。

### 6.3 预览传输

CLI 生成帧时经 JSON-RPC callback 传 JPEG 预览（512px 宽，display-ready）：

```
CLI --[RPC]--> Python run.py --[stdout DLSS_EVENT:]--> Electron main --[IPC]--> renderer
```

renderer 用 `atob → Blob → URL.createObjectURL` 显示。

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
├── cli/               # C++ Vulkan pipeline（改名 backend/）
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

## 9. 实施顺序（2026-08-28）

### 已完成 — Phase 1 后端主路径

统一 `Pipeline`（SR|RR → Tonemap → FG → Output）、FileWatcher 消费删除、ProRes 422 HQ 流式写盘、RR 只走质量档。实测 frames 300–350、R:→S:：

- RR 原生 100% FG 4x MOV（`…_rr_v1.mov`）
- RR Balanced 58% / 8 spp / adaptive off / FG 2x（`…_rr_v2.mov`）
- SR EEVEE Balanced 58% / FG 2x（`…_sr_v1.mov`）

blend 未保存。

### 下一会话先做（GUI 之前必须落地）— 已全部完成（2026-08-30）

1. **预览 JPEG 回读**：`preview_frame` 走 RPC base64；GUI 侧 latest-wins 解码到 canvas。
2. **DLL 加载**：双版本都锁 `resources/` 的 310.7.129.0 三份 feature DLL（拷到 exe 旁）；NGX core 由驱动提供，不打包 `_nvngx.dll`。
3. **打包**：electron-builder（`app/`，`npm run dist`）产出 NSIS 安装包（`.dcp` 关联）+ 便携 zip + win-unpacked；`app/scripts/pack-backend.mjs` 组装 `resources/backend/`（cli + `*.spv` + 锁定 DLL + `pipeline/` 脚本）。

### 再往后（明天这三项之后）

| 优先级 | 项 | 说明 |
|---|---|---|
| 中 | 最后几帧背景变脏 | 完整序列后再查，高于双趟/HDR |
| 低 | SR `native_res_buffers` 双趟 | Python 有 `color/`+`guides/`，C++ 只有一个 `inputDir` |
| 低 | HDR ProRes | 打了 PQ+BT.2020 标签，未跑过 |
| 预留 | NR / ProRes 4444 | 位和枚举在，不做 |

前端（Flutter 设置面板、Job 列表、把 JPEG 画到预览窗）等上面三项完成后再开。DESIGN §6 Electron 草案不要当施工图。

---

## 10. 已知问题 / 待确认

| 编号 | 问题 | 结论 |
|---|---|---|
| RR-P1 | RR Preset F 在 310.7.129 是 RR2 | 已用 129 + Preset F 跑通 300–350；驱动需 ≥580 |
| RR-P2 | Blender 5.3 Denoising Specular Motion | inject 已接线，正式测试用过 |
| SR-P1 | native_res_buffers 双趟 | Python 窗口有，C++ 没接。优先级同 HDR，后做 |
| FG-P1 | RR→FG 与 SR→FG | 都是 tonemap 后进 FG。已实测 |
| DLL-P1 | Release DLL 从哪来 | 随应用（exe 旁），NGX core 来自驱动；开发版锁 310.7.129 |
| PV-P1 | 预览 JPEG | 回读通路在，编码/RPC 未开。GUI 前必须做 |
| QA-P1 | 4x 原生片最后几帧背景变脏 | 完整拼完再查 |
