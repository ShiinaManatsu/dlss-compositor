# dlss5-video 第三方实现调研（Clinteastman/dlss5-video）

> 2026-08-30。仓库：https://github.com/Clinteastman/dlss5-video （MIT，无 NVIDIA 二进制/模型，用户自备 Streamline/ReShade/DLL）。
> 结论先行：**架构互证为主，无需改动我们现有管线**；值得记录的是它的预计算引导容器模式、时序 reset 语义、A/B 诊断思路，以及 310.8.0 Streamline runtime 的动向。

## 它是什么

把隐藏的 DLSS Neural Rendering（NGX "feature 18"，即 DLSS5 NR 泄露模型）套在**普通视频**上：`视频帧 → 深度估计 + 光流 → DLSS NR → 显示`。形态是 mpv 实时播放 + 注入，不是离线渲染农场：

- **注入层**：ReShade add-on（`dlss5-video-renderer.addon64`）钩住 mpv 的 D3D11 后备缓冲，桥接到常驻 D3D12 NGX 会话（仿 dlss5-dx11-bridge 的 D3D11→共享 D3D12 资源模式）。ReShade 同时负责呈现、overlay、A/B 旁路。
- **引导层**：伴生 Python/CUDA 进程——Depth Anything V2 Small（392² 推理 → 960×540 深度图，运动补偿时序平滑 + 稳定百分位区间抑制深度闪烁）+ NVIDIA Optical Flow（Fast 档、半分辨率、1×1 输出网格、temporal hints，**场景切换检测并 reset 时序状态**）。
- **传输**：共享内存单槽 latest-wins，"丢弃过期捕获机会而不是让播放阻塞在推理后面"；任何一级失败都必须保持旁路可用。
- **离线模式**：`Invoke-NeuralClip.ps1` 离线出 MP4，**跨帧复用同一个 NGX 会话**；`Estimate/Pack-VideoGuides.ps1` 可把深度+光流预计算打包成 `.d5gp` 文件（OpenCV DIS 作为光流 fallback）。
- **SR 细节**：`-Scale 1` 走原生尺寸/DLAA 路径；更大倍率按输入/输出尺寸**重建 NGX feature**，让 NGX 而不是播放器做放大。
- **运行时**：用户自备 **patched 310.8.0 Streamline runtime**（比我们锁定的 310.7.129.0 新）。他们的状态结论：**runtime 已通，质量瓶颈在引导与色彩输入**。

已知缺陷（他们自己列的）：灰脸/阴影发脏、未收敛纹理涟漪、视频深度时序不稳、色彩/曝光契约、引导-画面延迟。和我们调 RR 时遇到的脏迹/时序问题是同族。

## 与我们栈的对照

| 维度 | dlss5-video | 我们 |
|---|---|---|
| 会话 | 常驻 D3D12 NGX 会话跨帧复用 | 常驻 CLI 进程 + Vulkan NGX |
| 引导来源 | DAV2 深度 + NVOF 光流（外部估计） | Cycles 真值（denoising passes）+ RrChannelMapper |
| 帧调度 | 共享内存单槽 latest-wins，不阻塞 | 预取器 wait-for-files + 背压 + 外部停止 |
| 放大 | 按尺寸重建 NGX feature | 质量/百分比 → rebuild，同构 |
| 色彩 | 无契约，正在补测试 | inject.py 固定 ACES→LUT tonemap，天生有契约 |
| 注入 | ReShade 钩 mpv（游戏式实时） | 无注入，离线管线 + RAM 盘 watch |

## 借鉴价值（按优先级）

1. **架构互证**：常驻会话 + latest-wins 单槽 + 失败旁路 + 离线跨帧同会话——与我们 CLI 设计完全同构，方向得到第三方验证，不用改。
2. **预计算引导容器模式（`.d5gp`）**：若将来支持「外部视频/EXR 序列直接走 RR」（不经过 Blender），这就是蓝图：离线估计 depth+flow → 打包 → 逐帧喂 NGX，场景切换 reset 时序。现在不实现，记录格式思路。
3. **时序 reset 语义**：scene-cut 检测 + temporal hints reset。对应我们的 FG 历史帧管理；我们支持任意 frame_start/end 分段导出，段边界处 NGX 的 reset 语义值得确认一遍（pipeline 每窗口已有 reset 路径）。
4. **A/B 诊断**：blank-guide A/B、深度/流向预览、显式状态面板。低成本高价值的调试手段——GUI 将来可加「无引导 A/B」开关对比 RR 贡献。
5. **跟踪 310.8.0**：更新的泄露 Streamline runtime 已流通。DLL 锁定策略不变；若 310.8.x 的 feature DLL（nvngx_dlss/dlssd/dlssg）拿到手，只需换 `resources/` 重测，无需代码改动。
6. **色彩契约**：他们踩的坑验证了我们把色彩链路固定在 inject.py 里的做法；仅在做外部视频输入时才需要补契约测试。

## 明确不借鉴

- ReShade 注入路线：解决的是"在游戏/播放器里实时跑"，我们是离线渲染管线，无注入需求。
- NVOF / DAV2 引导估计：Blender 场景有真值引导，神经网络估计只在外部视频场景才有意义。
- mpv 集成 / YouTube 播放：与产品形态无关。
