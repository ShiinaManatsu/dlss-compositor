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

### 1.1 分辨率控制

- 插件面板提供**最终输出分辨率**输入（Width × Height）
- 横向 toggle 单选 **Quality Mode**：Ultra Performance → Performance → Balanced → Quality → DLAA（按 DLSS 预设顺序排列）
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

### 1.3 相机数据导出（静默）

- 导出改为 **handler 静默执行**：每次渲染完成后自动写入，无需用户手动操作
- 不再导出单独的 `camera.json`，改为写入总配置文件 **`dlssconfig.json`**

### 1.4 dlssconfig.json 格式

```jsonc
{
  "version": 1,
  "output_resolution": { "width": 3840, "height": 2160 },
  "render_resolution": { "width": 2227, "height": 1253 },
  "quality_mode": "Balanced",
  "fps": 30,
  "color_space": "scene_linear",       // "scene_linear" | "srgb" | "pq"
  "is_hdr": true,
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

```
┌─────────────────────────────────────────────────────────┐
│  1. AOV 加载 (预取线程)                                    │
│     ┌── Color (Combined.RGBA)                            │
│     └── Depth + Vector (MV)                              │
│                                                          │
│  2. GBuffer 生产 (Compute Shader)                         │
│     ┌── Albedo, Normals, Roughness (从 AOV 通道重建)       │
│     └── Motion Vectors (Blender → DLSS 格式转换)          │
│                                                          │
│  3. DLSS-SR Evaluate                                      │
│     ┌── pInColor, pInDepth, pInMotionVectors              │
│     ├── InJitterOffset (from dlssconfig.json)             │
│     ├── InMVScale = (1.0, 1.0)                            │
│     ├── IsHDR = true, AutoExposure = true                 │
│     └── InFrameTimeDeltaInMsec = 1000 / fps               │
│                                                          │
│  4. Tone Mapping (Post-DLSS)                              │
│     ┌── Scene-linear → display-ready (PQ or sRGB EOTF)    │
│     └── 仅在 SR+FG 路径或输出编码需要时                    │
│                                                          │
│  5. DLSS-FG Evaluate (可选)                                │
│     ┌── Display-ready backbuffer (tone-mapped)             │
│     ├── Depth + MV (渲染分辨率)                            │
│     ├── colorBuffersHDR = true                            │
│     └── camera matrices from dlssconfig.json              │
│                                                          │
│  6. 输出 (异步写队列)                                       │
│     └── EXR 写入不阻塞管线                                 │
└─────────────────────────────────────────────────────────┘
```

**关键纠正**（基于本轮文档审计）：
- **SR 在 TONE MAPPING 之前**（DLSS SR §3.1）
- **FG 在 TONE MAPPING + EOTF 之后**（DLSS FG §HDR）
- **InMVScale = (1.0, 1.0)**（MVLowRes=1 时 DLSS 内部缩放，不需要额外乘）
- **不再传 GBuffer**（标记 "research only"，Phase 2 已删除，本次重构不恢复）
- **无 "10-nit clamp"**（官文不存在，README 虚假信息）

### 2.4 性能优化

| 优化项           | 说明                                                  |
| ---------------- | ----------------------------------------------------- |
| **Texture 池化** | VkImage 跨帧复用，避免每帧 create/destroy             |
| **AOV 预取**     | 后台线程异步读取下一帧 EXR，GPU 处理不等待 IO         |
| **批量 Upload**  | 单 command buffer + 单 vkQueueSubmit 上传所有 texture |
| **异步写队列**   | EXR 写入走独立线程 + 队列，不阻塞渲染管线             |
| **内存预算**     | `--memory-budget` 控制 GPU 纹理预取上限               |

### 2.5 RPC 接口设计

```
// 任务管理
dlss/setTask          { dlssconfig_path, preset_override?, fg_enabled?, fg_multiplier? } → { ok, task_id }
dlss/checkReady       {} → { ready: bool, reason?: string }
dlss/startTask        { task_id } → { ok }

// 状态查询
dlss/getStatus        {} → { dlss_version, fg_max_multiplier, gpu_name, driver_version, is_hdr_supported }
dlss/getTaskState     { task_id } → { state: "idle"|"running"|"done"|"error", current_frame, total_frames, eta_seconds }

// 执行状态（轮询用于进度条 + 帧预览）
dlss/getProgress      {} → { current_frame, total_frames, state }
dlss/getPreviewFrame  { frame_index } → { width, height, format, data: base64 }

// 取消
dlss/cancelTask       {} → { ok }
```

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

5. **开始按钮**
   - 点击后通过 RPC 设置任务 + 启动

### 3.6 右侧容器 — 详情 + 预览

- 任务执行中显示实时帧预览
- 日志/错误信息
- 完成后显示统计（耗时、帧数）

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
- [ ] 主题/UI 设计（Flutter 端，本轮不讨论）
