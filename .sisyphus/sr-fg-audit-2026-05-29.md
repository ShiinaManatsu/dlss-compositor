# DLSS SR / FG 审计报告

**日期**: 2026-05-29  
**参考文档**:
- `docs/dlss-refs/DLSS_Programming_Guide_Release.md` (SR 310.5.0)
- `docs/dlss-refs/DLSS_FG_Programming_Guide.md` (FG 310.5.0)
- `docs/dlss-refs/NIS_SDK_Programming_Guide.md` (NIS 1.0.3, 仅供对比)
- UE5.7 DLSS 4.5 Plugin v8.6.0 (`D:\Github\2026.05.07_UE5.7_DLSS4.5Plugin_v8.6.0\Plugins\DLSS`)

---

## 🔴 BUG: InMVScale 双重缩放 → SR 时间累积失效

### 现状

```cpp
// sequence_processor.cpp:965-966
frame.mvScaleX = mvResult.scaleX * outputWidth / inputWidth;  // → 2.0
frame.mvScaleY = mvResult.scaleY * outputHeight / inputHeight; // → 2.0
```

### DLSS 文档 (§3.6.3)

> *"These should be set to 1.0 if motion vectors do not need to be scaled and should never be 0.0f."*

### DLSS 文档 (§3.6.2)

> *"NVSDK_NGX_DLSS_Feature_Flags_MVLowRes: Motion vectors are typically calculated at the same resolution as the input color frame (i.e. at the render resolution). [...] set the flag to '1'."*

### UE 插件参照

```cpp
// DLSSUpscaler.cpp:1070
DLSSArguments.MotionVectorScale = FVector2f::UnitVector; // (1.0, 1.0)
```

### 根因

`MVLowRes=1` 时，DLSS 内部根据 input/output 分辨率比自动缩放 MV。我们又额外乘 `(outputWidth/inputWidth, outputHeight/inputHeight)` = `(2,2)`，导致**双重缩放**：实际位移 4x → 时间重投影采样到完全错误的位置 → 历史累积无效 → SR 退化为纯空间上采样（≈ bilinear）。

### 为什么 FG 不受影响

FG 使用独立的 `mvecScale = (1/width, 1/height)`（归一化到 UV 空间），不走 `InMVScale`。所以 MV 问题只影响 SR。

### 修复

`sequence_processor.cpp` 三处 SR 路径（SR-only 第 965 行、SR+FG 第 1608-1609 行、FG 第 2113 行对应 SR eval 部分）：

```diff
- frame.mvScaleX = mvResult.scaleX * outputWidth / inputWidth;
- frame.mvScaleY = mvResult.scaleY * outputHeight / inputHeight;
+ frame.mvScaleX = 1.0f;
+ frame.mvScaleY = 1.0f;
```

---

## 🟡 之前分析修正

### Previous Output buffer (§3.3)

DLSS 编程指南 §3.3 第 5 条定义了可选的 "Previous Output buffer (used for frame history and accumulation)"。SDK 中对应 `NVSDK_NGX_EParameter_PreviousOutput`（`nvsdk_ngx_defs.h:592`），是一个命名参数，不在 eval params 结构体中。

- UE 插件：不设（同样依赖 DLSS 内部历史管理）
- 当前代码：不设
- **结论**：可选，非必须。UE 也不设，DLSS 内部管理历史。当前不加不影响功能。

### pInOutput 每帧清零

`sequence_processor.cpp:948` 每帧 `updateData(output, zeros)` 把 `pInOutput` 填零。UE 中输出是 RDG 临时资源（内容未定义），语义等效。DLSS 使用 `pInOutput` 仅作为写目标，历史由内部缓冲区管理。**不是 SR 没效果的根因，但浪费 GPU 带宽。**

---

## ✅ 已确认正确

| 项 | 配置 | 参考 |
|---|---|---|
| IsHDR | `true`（默认）| DLSS SR §3.1.2: 线性空间数据必须用 IsHDR=1 |
| MVLowRes | `true` | DLSS SR §3.6.2: MV 和 input color 同分辨率用 1 |
| AutoExposure | `true`（默认）| DLSS SR §3.10 |
| DepthInverted | 不设 | 深度是 NDC [0,1]，非倒置 |
| MVJittered | 不设 | 离线渲染 MV 不包含 jitter |
| Jitter 传递 | camera.json → InJitterOffset ✓ | Blender Halton sequence [-0.5, +0.5]，路径完整 |
| InReset | 仅首帧 + gap + camera cut | 非每帧为 1 |
| MV 符号转换 | `result = -blenderMv` | Blender Vector pass 存 prev→curr，DLSS 要 curr→prev，取反正确 |
| InFrameTimeDelta | `1000/sourceFps` | SR 使用；FG 无此字段（FG 通过 clipToPrevClip 推导时间） |

---

## 🟢 SR vs FG 管线位置

| | DLSS SR | DLSS FG |
|---|---|---|
| **管线位置** | 色调映射**之前**（SR §3.1） | 色调映射 + EOTF **之后**（FG §HDR） |
| **输入颜色空间** | 场景线性 HDR（`IsHDR=1`） | Display-ready，PQ EOTF `[0,1]`（`colorBuffersHDR=true`） |
| **输入格式** | R16G16B16A16_SFLOAT | 推荐 RGB10A2（FG 文档）；当前用 R16G16B16A16_SFLOAT |
| **Delta Time** | `InFrameTimeDeltaInMsec`（毫秒） | 无此字段，通过 `clipToPrevClip` 矩阵推导时间 |
| **Jitter** | 需要（`InJitterOffsetX/Y`） | 不适用（FG 硬编码 0） |

**SR+FG 管线方向**：SR（场景线性入→场景线性出）→ PQ/tonemap → FG（display-ready 入→插帧出）。顺序正确，不可逆。

---

## 🔴 README 虚假信息

根目录 `README.md` 声称 *"DLSS Frame Generation internally clamps HDR color buffers to approximately 10 nits"* —— **官方 DLSS-FG 文档（310.5.0，4008 行）中不存在此描述**。FG 真正的要求是：输入必须有 tone mapping + EOTF，为 display-ready 格式（FG §HDR）。

PQ transport encoding 对 FG 是合理的（PQ 是 FG 推荐格式 HDR10 的 EOTF），但理由不是 "~10 nit clamp"，而是 FG 要求 display-ready 输入。

---

## ⚠️ 抖动相位数

`blender/dlss_compositor_aov/__init__.py:104`:
```python
jitter_phases = max(1, int(round(8.0 * scale * scale)))
```
2x → 32 相位，遵循 NVIDIA §3.7.1.1 `Total Phases = 8 × (Target/Render)²`。对离线批量处理器可能过多（动画通常 < 32 帧就循环了），但对 DLSS 功能无影响——DLSS 只关心每帧特定的 jitter 值，不关心相位总数。

---

## 待评估项

- **pInOutput 清零可否跳过**：非根因，但每帧做 CPU→GPU 传输拷贝零值浪费带宽。可改为仅首帧/reset 时初始化。
- **FG backbuffer 格式**：FG 文档推荐 `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (RGB10A2)，当前用 `R16G16B16A16_SFLOAT`。功能可能正常，但 FG 可能在 RGB10A2 上有硬件加速路径。
