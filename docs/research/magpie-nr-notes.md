# Magpie DLSSNR 调研（SAOG0721/Magpie @ 8dfdf37）与 NR pass 接入设计

> 2026-08-30。对照：dlss5-video（docs/research/dlss5-video-notes.md，DLSS5 NR = NGX "feature 18" 泄露模型）。
> 结论先行：Magpie 的 DLSSNR 就是 dlss5-video 生态里的那个神经降噪——**NGX feature ID 18，`nvngx_dlssnr.dll` snippet**，
> 同分辨率、显示域（SDR RGBA8），MV+深度引导，参数全部走 `"DLSSNR.*"` 字符串键。
> 我们的接入点：**Tonemap 之后、FG 之前**（与猜测一致，见下）。

## Magpie 侧事实（读源码核实，非 commit 页摘要）

两套东西都存在，注意区分：

1. **`DLSSNRFilter.cpp` = NGX feature 18 路线**（真正的 DLSSNR）
   - 优先直接 `LoadLibrary("nvngx_dlssnr.dll")` 拿 `NVSDK_NGX_D3D12_Init_Ext / CreateFeature / EvaluateFeature / ReleaseFeature`
     （带 IAT hook 把 caller 伪装成 nvngx.dll，绕 snippet 的 caller 校验）；
     core NGX `CreateFeature(feature 18)` 仅作诊断（`ENABLE_CORE_FEATURE18_DIAGNOSTIC=false`，
     注释："must never run before the signed snippet in a production session"）。
   - **创建参数**：`DLSSNR.Width/Height`、`DLSSNR.InputWidth/InputHeight`、`DLSSNR.OutputWidth/OutputHeight`（全部=同分辨率）、
     `DLSSNR.Upscaling=0`、`DLSSNR.Scale=1.0f`、`DLSSNR.ScalingRatio=1.0f`、`DLSSNR.Hint.Render.Preset=1`（不对外暴露）、
     外加 stock `Width/Height/PerfQualityValue(Balanced)/NodeMask`。
   - **评估参数**：`DLSSNR.Color` / `DLSSNR.Output`（RGBA8_UNORM 整资源指针）、`DLSSNR.MVec` / `DLSSNR.Depth`（引导资源）、
     每资源 subrect 四键（`DLSSNR.MVecSubrectBaseX/BaseY/Width/Height` 等）、`DLSSNR.MVecScaleX/Y=1.0`、`DLSSNR.DepthInverted=1`、
     `DLSSNR.Style`（0 Default/1 Natural/2 Cinematic）、`DLSSNR.Intensity`、`DLSSNR.LocalToneStrength`、
     `DLSSNR.LocalStructureStrength`、`DLSSNR.UseAutoMask`、stock `Reset`（首帧或引导元数据要求时置 1）。
   - **格式契约**：输入输出必须 RGBA8/BGRA8_UNORM（SDR 8-bit 显示域）；`PARAM_UI_CORRECTION=0`。
   - **时序状态**：首帧 `Reset=1`；引导切换/场景变化经 guidance metadata OR 进 Reset；失败 → 置 disabled 走 passthrough
     （这是 Magpie 的备胎，我们不要）。
   - **guidanceMode**：0=全引导（MV+Depth）、1=Force Zero（无引导）、2=Motion Only、3=Depth Only。
2. **`RTXVideoDenoiser.cpp` = NvVFX（Maxine Video Effects）路线**，与 NGX 无关：
   `NvVFX_CreateEffect("VideoSuperRes")` + `QualityLevel`（1–4 放大 / 8–11 降噪），D3D11+CUDA interop，**无 MV/深度引导**。
   对我们没有借鉴价值（引导质量正是我们的优势）。

## 与 dlss5-video 对照

| 维度 | dlss5-video | Magpie DLSSNR | 我们可拿到的 |
|---|---|---|---|
| 模型入口 | patched 310.8.0 Streamline runtime | `nvngx_dlssnr.dll` snippet（feature 18） | 同一 snippet DLL |
| 引导 | NVOF 光流 + DAV2 深度（估计） | FrameGuidance（NVOF/DAV2/零引导可选） | **Cycles 真值 MV/Depth**（更好） |
| API | D3D12 NGX | D3D12（snippet 直调或 core） | **Vulkan**（未验证！见风险） |
| 域 | 显示域视频 | SDR RGBA8 显示域 | tonemap 后 display-ready |

## 接入设计（experiment/nr-pass 分支）

### 位置：Tonemap → **NR** → FG

理由：
1. **色彩契约**：NR 是显示域 SDR 8-bit 降噪器；我们 tonemap 后的 display-ready 帧正好满足。放在 SR/RR 之前会破坏
   scene-linear 管线（且违反契约，重蹈 dlss5-video 的色彩坑）。
2. **同分辨率**：NR 不放大；tonemap 后的帧已在输出分辨率，天然同分辨率。
3. **FG 之前**（时间一致性）：FG 插值帧没有自己的 camera 时间戳，若 NR 放在 FG 之后，插值帧会打乱其时序累积；
   NR 只吃真实帧（时间单调递增）然后 FG 在干净帧上插值，插值输出也继承干净画面。Magpie 的效果链顺序同样是
   NR（same-res filter）在 upscaler/FG 之前。
4. **用户猜测一致**：「感觉上是在 FG 前」——正确。

### Vulkan 可达性（已验证 DLL 侧，运行时待探针）

**DLL 事实（2026-08-30，Magpie Experimental release 的 `nvngx_dlssnr.dll`，FileVersion 310.8.0.0，md5
74a49810dc53056e3b9fe2b3d0afcbc6）**：导出表含 **`NVSDK_NGX_VULKAN_C/E/G/I/P/R...`（Vulkan 全套）** +
D3D11/D3D12/CUDA 全 API——VK 路线在 DLL 侧成立。已拷入仓库 `resources/`（gitignored，不随安装包分发，
pack-backend 是固定三 DLL 列表）+ cli.exe 旁供实验。310.8.0.0 与锁定 310.7.129.0 三 DLL 混用：feature DLL
由 NGX core 各自独立加载，跨 feature 异版本无碍；若运行时行为异常再回退。

### 已实现（本分支）

- `kNR = 0x08`（pipeline.h 预留位）激活：Pipeline 加 NR 段（tonemap 后、FG 前），fail-loud：要求 NR 但 feature
  创建失败 → 直接 RunEvent::Error（无备胎）。
- `DlssNrPass`（cli/src/dlss/dlss_nr.*）：raw NGX VK create/evaluate，`DLSSNR.*` 全参数，guidanceMode 支持。
- `nr_pack_rgba8/nr_unpack_rgba8` compute pass：rgb10_a2 ↔ rgba8 转换（NR 的格式契约）。
- RPC start_job 参数：`nr`（bool）、`nr_style`（0/1/2）、`nr_intensity`（0–1）、`nr_guidance`（0–3）。
- NR 输出 fed 给 FG / 输出 / 预览；EXR 反向 tonemap 链不变。

### 风险 / 待验证

1. VK 导出是否存在（核心风险，探针即答案）。
2. 引导 subrect < 全分辨率是否被接受（我们 MV/Depth 在 render res，NR 在 output res；Magpie 用 validRegion 传过 subrect，
   理论支持）。
3. HDR：NR 契约是 SDR；HDR 任务（PQ 10-bit）先不支持——HDR+NR 组合在 validate 拒绝。
4. snippet 的 caller 校验在「NGX core 加载」路径下是否自然通过（Magpie 直调才需要 hook）。
5. 需要自备 `nvngx_dlssnr.dll`（dlss5-video/Magpie 社区发行版），放 cli.exe 旁；不进 git、不进安装包，锁定策略同三 feature DLL。
