# DLSS Compositor v2 — Greenfield Rewrite Plan

## TL;DR

> **Quick Summary**: 全新 v2 分支三层架构 greenfield 重写 — Blender 插件(配置 + handler 静默导出 dlssconfig.json)+ Flutter Windows native plugin GUI(MethodChannel/EventChannel/FFI Pointer 与 C++ 引擎通信)+ Vulkan/NGX-direct DLSS 引擎(SR+FG 离线批渲)。严格按 NVIDIA DLSS 310.6.0 文档实现,**禁止 UE 代码 cargo-cult 抄袭**。
>
> **Deliverables**:
> - Blender 5.1+ 扩展(panel header enable toggle + Quality Mode 横排 + Output Dir + Apply Settings 配置 pass+compositor;**handler 在 render 时 transparent save→override→restore `resolution_percentage`**;Halton(2,3) base=8 jitter + 静默 handler 在渲染结束/中断时合并导出 `dlssconfig.json`)
> - Flutter Windows 桌面 GUI(任务配置 + 实时预览帧 + 进度,native plugin via FFI + 共享内存零拷贝)
> - C++ DLSS 引擎(`dlss_engine_lib` 静态库,链入 plugin DLL 也可由 `dlss-compositor-cli.exe` 包出 JSON-RPC fallback)
> - SR-only / FG-only / SR+FG 三种管线流程
> - 输出:scene-linear EXR(R16G16B16A16_SFLOAT)+ ProRes 422 HQ via 静态链接 FFmpeg
> - 中英双语单文件 `docs/user_manual.html`(端用户面向)
> - DLSS submodule 升级到 v310.6.0
>
> **Estimated Effort**: XL(42 个 task,T25 CAS 已删 per 用户决议 round 2;跨 Blender/Flutter/C++ 三技术栈,~3-5 周全职等价)
>
> **Parallel Execution**: YES — 8 waves(Wave 0 串行 + Wave 1-5 内大并行 + Wave 6 串行 + Wave 7 并行 + Final 4 并行)
>
> **Critical Path**: 1 → (3 + 5) → (17 + 18 + 21 + 22 + 23) → (24 + 27) → 36 → 38 → F1-F4 → user okay

---

## Context

### Original Request
v1 因为 agent 不读 DLSS 文档、照着 UE 插件改导致管线一堆坑(InMVScale 双重缩放、pInOutput 每帧清零、README 假"10-nit clamp"等)。v2 新分支 greenfield 重写,严格参照 DLSS 文档落地,聚焦管线设计:从 EXR 输入 → SR/FG 整合 → EXR/ProRes 输出。同时 Blender 插件做生命周期管理、handler 静默导出 dlssconfig.json,避免快速渲染时多 frame 写竞态。Flutter 替代 Electron 前端,VSCode F5 调试便利,打包后可测试,完整日志系统。

### Interview Summary

**用户决议(5 项关键)**:
| 项 | 决定 |
|---|---|
| Jitter base | **8**(DLSS-compliant per §3.7.1.1)。Blender 插件 Halton(2,3),按 quality 给 phase 数(72/32/24/18/8) |
| 通信方案 | **Flutter native plugin 主路径**(MethodChannel for commands + EventChannel for events + FFI Pointer 共享内存为预览帧零拷贝)。Windows-only。JSON-RPC stdio fallback target 同建,实现失败才启用 |
| 代码存放 | 新分支 `v2/main` greenfield,只保留 `DLSS/` submodule(需 DLL + helper headers) |
| 视频输出 | **ProRes 422 HQ + EXR**,留扩展口(DNxHR / 4444 XQ 不在 v2) |
| 测试纪律 | **Tests-after + 强制 Agent QA scenarios**(每 task 必须含 happy + 至少一个 failure scenario) |

**Quality Mode → render resolution 绑定**(用户 Q1 + Q-toggle 决议):用户在 Blender 标准 Render Properties 设输出 W×H + 在 plugin panel 选 Quality Mode + 启用 plugin(panel header checkbox);**plugin handler 在 `render_init` 时 transparent save 用户原 `resolution_percentage` 并 override 为 `round(100 / DLSS_RATIO[mode])`,在 `render_complete/render_cancel/load_post` 时还原**;`dlssconfig.render_resolution` 用 Blender 实际整数 `resolution_x * percentage / 100`(避 float drift)。**不暴露细粒度 scale 给用户**;**Apply Settings 按钮只配置 pass/compositor/denoise/OCIO,不改 percentage**。

**FG output naming**(用户 Q2):重编号连续序列(2x → `0001..0(2N-1)`,4x → `0001..0(4N-3)`),与渲染输入分目录隔离。

**Camera cut detection**(用户 Q3):**矩阵推导主路径 + MV pass 兜底**,**不依赖 Blender `cut_after` 字段**(纯 CLI 端推断)。

**Test fixtures**(用户 Q4):camera.json / static_camera.json 通用 fixtures cherry-pick;EXR 序列 fixtures 重生成。

**DNxHR**(用户 Q5 → round 2 修订):**v2 不实现**(EncoderRegistry 不注册、CLI/GUI 不暴露、JSON schema `output_format` enum 拒绝 `"dnxhr"`);**enum 占位 + docs Future Extensions 章节允许提及**(per round 2 "留扩展口 + 记录到文档")。多维度策略统一表见 Must Have 末尾"扩展 codec 多维政策表"。

### Research Findings

| 调研主题 | 结论(供 plan 引用) |
|---|---|
| **Flutter Windows native plugin 可行性**(`bg_36a24283`) | **GO**。MethodChannel for commands(lock-free atomics for getProgress < 10ms)+ EventChannel for events/progress(<1KB)+ **FFI Pointer + 共享内存** for 预览帧(零拷贝,8MB×4fps OK)。C++ 后台 std::thread,callback **必须 post 到 `engine->ui_task_runner()`**。VSCode F5 compound launch.json(Flutter Dart + cppvsdebugger attach)双调试已验证可行。生产参考:`win32`(5.3M 下载,纯 FFI)、`flutter_local_notifications_windows`(FFI + EventChannel 混合)、`biometric_storage`。 |
| **JSON-RPC stdio C++ 实现**(`bg_03036ae9`) | Content-Length framing(LSP 标准,binary-safe);自写 ~300 LOC 在 `nlohmann::json` 之上(无现成 C++ 库可用,clangd/rust-analyzer 都自写);I/O 线程 + 渲染 worker 线程,lock-free 进度结构;Windows 必须 `_setmode(_O_BINARY)` 防 CRLF 转换破坏 Content-Length。 |
| **UE5.7 DLSS-SR NGX-direct 调用模式**(自查 UE 源码) | `DLSSFeatureFlags = IsHDR \| MVLowRes \| (DepthInverted if reverse-Z) \| (AutoExposure if cvar) \| (AlphaUpscaling 可选)`。`MotionVectorScale = UnitVector` (1,1)。`bReset = bCameraCut OR !PreviousHistory`。`DeltaTimeMS = WorldDelta * 1000`。`InMVSubrectBase = (0,0)` for 全帧 MV。Per-quality preset 单独读 `UDLSSSettings`,默认 `EDLSSPreset::Default`(让 NVIDIA 决策)。**仅作 cross-check,正式实现以 DLSS doc 为准**。 |
| **UE DLSSMoviePipelineSupport 离线批渲** | **没有离线专属 jitter/MV/reset 处理** — 只是设 `r.ScreenPercentage` + 注册 view extension,完全复用 UE engine 标准 upscaler 路径。**对我们的启示:v2 必须自己驱动 jitter**(已规划,Blender 插件中实现)。 |
| **DLSS-G v310.6.0 Vulkan eval 字段**(自查 SDK 头文件) | Required: `pBackbuffer + pDepth + pMVecs + pOutputInterpFrame`;Optional 6 个全 nullptr 并在 Create 时设 `ResourceNeverProvided_Flags = HUDLess\|UI\|UIAlpha\|BidirectionalDistortionField` 省 VRAM。 |
| **DLSS submodule v310.6.0 tag**(`git ls-remote`) | ✅ 已验证存在(`cb017647...`)。310.5.3 → 310.6.0 唯一影响:删除 `pNoPostProcessingColor` struct field;新增 `ResourceAlwaysProvided/NeverProvided_Flags`。 |

### Metis Review

Metis 找出 10 个 blocking 项,**全部已消化**:
- 5 个 user 决议(Q1-Q5,见 Interview Summary)
- 5 个我自动解决:
  1. **mvecScale 修正**:`(1.0, 1.0)` for pixel-unit MVs(per FG §6 line 591-593),不是 `(1/W, 1/H)`(`v2-refactor-goals.md §1.5` 错误注释)
  2. **MV channel mapping 推导**:Blender Vector pass(X 正=左, Y 正=下, prev→curr) + DLSS(X 正=右, Y 正=下, curr→prev) → **`DLSS_MV = (Blender.X, -Blender.Y)`**(仅 Y 取反;X 因 Blender 左-正约定 + 时间反转两次符号抵消而直通)。**与 v1 audit `-blenderMv`(双轴取反)冲突**,v1 可能因 InMVScale 双重缩放掩盖了 X 轴方向错误 → **T23 必须含实证测试场景验证**
  3. **Debug overlay programmatic control**:DLSS SR §8.2 确认仅 dev DLL + 键盘 hotkey,headless 不可用 → T34 不实现 toolbar
  4. **FFmpeg static link**:vcpkg `x64-windows-static-md` triplet,prores_ks 在默认 build 即可用
  5. **DLSS NGX call sites 强制注释**:每个 `NVSDK_NGX_*` 参数赋值必须有 `// DLSS §X.X` 引用注释(防止 UE 抄袭式 bug 重现)

详见 `.omo/drafts/dlss-compositor-v2.md` 的 "Metis Round 1 Resolutions" 章节。

---

## Work Objectives

### Core Objective
Greenfield 重写 DLSS Compositor v2 — 三层架构(Blender 插件 + Flutter native plugin GUI + Vulkan/NGX-direct C++ 引擎),严格遵循 NVIDIA DLSS 310.6.0 SR/FG Programming Guide,实现 Blender Cycles EXR 离线批渲的 SR(超分辨率)+ FG(帧生成)处理管线,输出 EXR/ProRes 422 HQ。

### Concrete Deliverables
- **Blender 扩展**(`blender/dlss_compositor/`):quality mode UI、Halton(2,3) jitter base=8、Configure All Passes 按钮、render handler 静默写 per-frame `frames/<N>.cam.json` + 渲染结束 atomic merge 到 `dlssconfig.json`
- **Flutter Windows 桌面 GUI**(`gui/`):dlssconfig 选择器 + 任务配置面板 + 实时预览帧(2-4 fps,8MB)+ 进度条 + 完成统计 + Win32 FileOpenDialog FFI
- **C++ DLSS 引擎**(`src/`):
  - `dlss_engine_lib` 静态库(Vulkan compute-only + NGX-direct + 7-stage 管线)
  - `dlss-compositor-plugin.dll`(Flutter native plugin via FFI + MethodChannel + EventChannel + 共享内存预览帧)
  - `dlss-compositor-cli.exe`(JSON-RPC stdio fallback,同一份 lib + main + Content-Length framing)
- **DLSS submodule** 升级到 `v310.6.0` tag
- **测试**:Catch2 (C++)、flutter_test/integration_test (Flutter)、pytest + bpy mock (Blender)
- **CI**:GitHub Actions Windows runner(submodule recurse + cmake build + ctest + flutter test)
- **文档**:`README.md` / `docs/build_guide.md` / `docs/blender_export_guide.md` / `docs/gui_guide.md` / `docs/cli_reference.md` / **`docs/user_manual.html`(端用户中英双语单文件)**
- **样例 Blender 场景** + 端到端 QA fixture

### Definition of Done
- [ ] `git checkout v2/main && git submodule update --init --recursive && cmake -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=DLSS && cmake --build build --config Release` 一次成功
- [ ] `ctest --test-dir build --config Release --output-on-failure` 全过
- [ ] `cd gui && flutter test && flutter test integration_test` 全过
- [ ] `python -m pytest blender/tests/` 全过
- [ ] 端到端 QA: 加载样例 Blender 场景 → render → 启动 Flutter → 选择 dlssconfig.json → SR(scale 2)+FG(2x) → 输出目录有 `2N-1` 个 EXR + 1 个 ProRes 422 HQ MP4
- [ ] 双击 `docs/user_manual.html` 浏览器打开,中英双栏内容完整
- [ ] 4 个 Final Wave reviewer 全部 APPROVE
- [ ] 用户 explicit "okay"

### Must Have
- **每个 `NVSDK_NGX_*` 参数赋值**必须有 `// DLSS §X.X` 文档引用注释(防 UE 抄袭式 bug 重现)
- DLSS submodule 升级到 v310.6.0 并删除 `pNoPostProcessingColor` 引用
- SR Feature Flags 永远开 `IsHDR | MVLowRes | AutoExposure`,**不开** `DepthInverted`(Blender NDC 非反转)
- SR `InMVScale = (1.0f, 1.0f)`(per UE 与 DLSS doc §3.6.3)
- FG `MvecScaleX/Y = (1.0f, 1.0f)`(per FG §6 L591-593,MVs 已在 pixel units)
- FG `pBackbuffer` 格式 = `VK_FORMAT_A2B10G10R10_UNORM_PACK32`(RGB10A2,per FG §HDR L1462-1463)
- FG Create 时 `ResourceNeverProvided_Flags = HUDLess|UI|UIAlpha|BidirectionalDistortionField` 省 VRAM
- Halton(2,3) jitter base=8,phase counts per quality:`DLAA=8 / Quality=18 / Balanced=24 / Performance=32 / UltraPerformance=72`(per SR §3.7.1.1)
- Jitter pixel space `[-0.5, +0.5]`,Blender 写入前 clamp `[-0.499, +0.499]` 防越界
- MV 通道映射 `DLSS_MV = (Blender.X, -Blender.Y)`(T23 实证验证后锁定)
- SR 在 tone mapping **之前**(§3.1);FG 在 tone mapping + EOTF **之后**(§HDR)
- SR 输入:scene-linear HDR(`R16G16B16A16_SFLOAT`,IsHDR=1)
- FG 输入 backbuffer:display-ready PQ-encoded(HDR project)或 sRGB-encoded(LDR project)
- SR + FG 共享同一份 GPU depth/MV texture(per FG §6 L544-545)
- `InReset` 触发条件:首帧 || 帧 gap > 1 || matrix discontinuity 推导
- `InFrameTimeDeltaInMsec = 1000.0f / dlssconfig.fps`(不硬编码 33.3f)
- Per-quality preset 自动映射:DLAA/Quality/Balanced/UltraQuality → K;Performance → M;UltraPerformance → L(per DLSS §3.12)
- FG 3x/4x:`evaluate` 调 `(multiplier-1)` 次 per input frame(per FG §1 L518)
- 所有 NGX 调用走**直接 NGX**(不走 Streamline)
- Blender 插件 handler 在 `render_init` transparent save 用户原 `resolution_percentage` + override 为 quality 推导整数;`render_complete/render_cancel/load_post` 时还原(per 用户决议:渲染期透明介入,渲染后无感还原)。`dlssconfig.render_resolution` 用 Blender 实际整数 `resolution_x * percentage / 100`(避 float drift)
- Blender 插件 handler 用 per-frame `frames/<N>.cam.json` + 渲染结束 atomic merge 到 `dlssconfig.json`(防快速渲染竞态)
- Blender 插件强制 File Output node 的 color management 为 `OVERRIDE` + view transform = `Raw`(Blender 5.1 OCIO 术语,scene-linear no view transform,per [Blender docs](https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html));`is_hdr` 由 `display_device` 推导(BT.2100-PQ/HLG → true,sRGB → false),不暴露给用户手动切换
- C++ 双 build target:`dlss_engine_lib` (static) + `dlss-compositor-plugin.dll` (Flutter FFI) + `dlss-compositor-cli.exe` (JSON-RPC fallback)
- C++ 后台 std::thread 调 channel callback 必须 post 到 `engine->ui_task_runner()`(per Flutter Windows 线程模型)
- Schema single source of truth:`core/dlssconfig.schema.json` 由 T5 产出,T6/T15/T32 都引用
- spdlog 输出:stderr + 文件 sink,**绝不写 stdout**(stdout 留给 channel/JSON-RPC)
- **每个 task 必须含 Agent QA scenarios**(happy + 至少 1 个 failure 场景)

### Must NOT Have (Guardrails)
- **不照搬 UE 代码**:UE 仅作 cross-check,实现以 DLSS doc 为准。任何 UE 派生代码必须有对应 DLSS doc 引用注释
- **不设** `InSharpness`(SR §3.11 已废弃;DLSS 模型已训练为产出 sharp 图像,无需外部锐化;v2 不实现 CAS pass)
- **不设** `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`(Blender NDC 非反转)
- **不在每帧设** `InReset = true`(只在首帧 / gap / camera cut)
- **不硬编码** `InFrameTimeDeltaInMsec = 33.3f`(从 dlssconfig.fps 推导)
- **不在 CLI 端重算 jitter**(Blender 是 jitter 源)
- **不在每帧 vkCreateImage**(全部走 TexturePool T19)
- **不向 stdout 写人类可读文本**(stdout 留 RPC/channel,log 走 stderr/文件)
- **不在 SR 之前 tonemap**(违反 §3.1)
- **不设** `pOutputRealFrame`(live game 用,offline 不需要)
- **不引入** DX12 / Streamline / NIS / DLSS-RR
- **不重建 GBuffer**(标 "research only")
- **不实现 DNxHR / ProRes 4444 XQ / JPEG / H.264 / H.265 / AV1 / AVIF / WebP**(per 用户 round 2 最小支持决议;v2 实现期仅 EXR + PNG 16-bit (LDR) + ProRes 422 HQ;扩展项作为 enum 占位 + docs 提及未来计划,EncoderRegistry 不注册)

### 扩展 Codec 多维政策表(unified per Momus round 1)

| 维度 | DNxHR / ProRes 4444 XQ / H.264 / H.265 / AV1 / AVIF / JPEG | 实现的 EXR / PNG 16-bit (LDR) / ProRes 422 HQ |
|---|---|---|
| **C++ enum (`OutputFormat`)** | 占位 enum value 允许(标 `/* not implemented in v2 */`) | 实际枚举值 |
| **EncoderRegistry 注册** | **禁止注册**(F1/F4 grep `register("dnxhr"\|"h264"\|...)` → 0) | 注册 |
| **JSON schema (`dlssconfig.output_format`)** | **拒绝**("dnxhr"/"h264"/etc 不在 enum) | 接受 |
| **RPC `getStatus().video_encoders / export_formats_*`** | **不列** | LDR `["png_16","exr","prores_422hq"]`;HDR `["exr","prores_422hq"]`;video `["prores_ks"]` |
| **GUI dropdown items** | **不列**(ast_grep DropdownMenuItem labels) | 列 |
| **CLI `--output-format`** | **拒绝** | 接受 |
| **Docs "Implemented / Supported" 章节** | **不宣称** | 列 |
| **Docs "Future Extensions / Roadmap" 章节** | **允许提及** | 不重复 |
- **不保留 Electron / 旧 CLI / camera.json**(随 v2 分支替换退役)
- **不引入** macOS / Linux 移植
- **不实现 debug overlay GUI toolbar**(SR §8 仅 dev DLL + hotkey,headless 不可用)
- **不依赖 Blender 写 `cut_after` 字段**(camera cut 由 CLI 端 matrix discontinuity 推导)
- **不用 `1 × scale²` jitter base**(违反 §3.7.1.1,正确是 `8 × scale²`)

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** — 所有验证均由 agent 自动执行。"用户手动确认/测试"类 acceptance criteria 被禁止。

### Test Decision
- **Infrastructure exists**: NO(v2 greenfield,从 0 建)
- **Automated tests**: YES(Tests-after + 强制 Agent QA scenarios)
- **Frameworks**:
  - C++ → **Catch2**(via FetchContent in T3 CMakeLists)
  - Flutter → **flutter_test** + **integration_test**
  - Blender → **pytest + bpy stubs**(`bpy-stubs` at `C:\Python313\Lib\site-packages\bpy-stubs\__init__.pyi`,**用户已装**;或 `pip install fake-bpy-module` 作 fallback)
- **CI**: GitHub Actions Windows runner(T10),build + ctest + flutter test 全过

### QA Policy
每个 task **必须** 包含 Agent QA scenarios(至少 1 happy path + 1 failure/edge case)。Evidence 保存到 `.omo/evidence/task-{N}-{slug}.{ext}`。

- **C++ CLI** → **Bash + curl/spawn**:`dlss-compositor-cli.exe --config sample.json` + 用 OpenEXR Python(或 `imageio`)读输出 EXR 验证 pixel hash / 帧数 / 分辨率
- **Flutter UI** → **flutter_test integration**(Playwright 不适用 Flutter)+ 启动 app + 模拟 click + 断言 widget 状态
- **Blender 插件** → **pytest + bpy mock**:register / handler 触发 / json 内容验证(jitter 范围、render_resolution 一致性)
- **管线集成** → 端到端:加载样例 Blender → render → CLI → 输出 → 像素对比

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 0 (Sequential - branch setup, 1 task):
└── 1: 新分支 v2/main 创建 + 清空除 DLSS submodule 外所有文件 [quick]

Wave 1 (After Wave 0 - infrastructure, 9 parallel):
├── 2:  DLSS submodule 升级 310.5.3 → 310.6.0 [quick]
├── 3:  根 CMakeLists.txt + vcpkg 配置 [unspecified-high]
├── 4:  spdlog 集成(stderr + file sink) [quick]
├── 5:  core/ types + dlssconfig.schema.json [quick]
├── 6:  core/ dlssconfig 加载 + validate [quick]
├── 7:  Blender 插件 v2 骨架(__init__ / manifest / AOV register) [unspecified-low]
├── 8:  Flutter app + plugin_ffi 脚手架(Win32 file dialog FFI) [unspecified-low]
├── 9:  .vscode/launch.json compound 双调试 [quick]
└── 10: GitHub Actions CI workflow [unspecified-low]

Wave 2 (After Wave 1 - Blender 插件):
  Sub-wave 2a (parallel - foundation): 11, 12, 13
    ├── 11: Resolution + Quality Mode UI 面板 [visual-engineering]   (deps: 5, 7)
    ├── 12: Halton(2,3) jitter base=8 实现 [ultrabrain]               (deps: 7)
    └── 13: Configure All Passes 按钮 + Vector pass 实证 sanity test [deep] (deps: 7)
  Sub-wave 2b (after 12): 14
    └── 14: render handler 静默 + atomic merge [unspecified-low]      (deps: 7, 12)
  Sub-wave 2c (after 11+12): 15
    └── 15: dlssconfig.json writer + schema validation [unspecified-low] (deps: 5, 11, 12)
  Sub-wave 2d (after all 11-15): 16
    └── 16: pytest + bpy mock infrastructure 完整搭建 [unspecified-low] (deps: 7, 11-15)

Wave 3 (After Wave 1 - CLI 引擎核心):
  Sub-wave 3a (parallel - foundation): 17, 20
    ├── 17: Vulkan context(volk + VMA, compute-only) [ultrabrain]    (deps: 3)
    └── 20: FramePrefetcher(async EXR read) [unspecified-high]        (deps: 3, 5)
  Sub-wave 3b (after 17 + 20): 18, 19, 21
    ├── 18: NGX init wrapper(SR + FG, dev/release DLL switch) [deep]  (deps: 17)
    ├── 19: TexturePool + uploadBatch [unspecified-high]              (deps: 17)
    └── 21: AOV channel mapper [unspecified-low]                      (deps: 5, 20)
  Sub-wave 3c (after 19): 22, 23
    ├── 22: Depth conversion compute shader [unspecified-low]         (deps: 3, 17, 19)
    └── 23: MV conversion compute shader + 实证 sanity test [deep]    (deps: 3, 17, 19, 13's MV_SANITY_RESULT)

Wave 4 (After Wave 3 - Pipeline stages):
  Sub-wave 4a (parallel): 24, 26, 29
    ├── 24: Stage 2 — DLSS-SR Eval 封装 [deep]
    ├── 26: Stage 4 — Tonemap + EOTF compute shader [unspecified-low]
    └── 29: Stage 7 — Async writer + multi-format encoder [unspecified-high]
  Sub-wave 4b (after 26): 27, 28
    ├── 27: Stage 5 — DLSS-FG Eval + 多帧 eval 循环 [deep]
    └── 28: Stage 6 — Inverse Tonemap compute shader [unspecified-low]
  (Note: T25 CAS sharpen REMOVED per user decision round 2 — DLSS model produces sharp output natively)

Wave 5 (After Wave 1 + 30 完成接口稳定后, 6 任务):
├── 30: Flutter native plugin bridge(MethodChannel + EventChannel + FFI Pointer + 共享内存) [deep]
└── 然后并行(30 接口稳定后):
    ├── 31: dlss/* RPC 接口实现(双 transport 共用 RpcDispatcher) [unspecified-high]
    ├── 32: Flutter 状态栏 + dlssconfig 选择器 [visual-engineering]
    ├── 33: Flutter 任务配置面板 [visual-engineering]
    ├── 34: Flutter 详情面板(预览帧 + 进度 + 日志) [visual-engineering]
    └── 35: SharedPreferences 持久化 + debug/release 配置分离 [unspecified-low]

Wave 6 (After Wave 4 + 5 - 管线拼装, 3 sequential):
├── 36: SR-only 主流程拼装(Stage 1 → 2 → 7) [deep]
├── 37: FG-only 主流程拼装(Stage 1 → 4 → 5 → 6 → 7) [deep]
└── 38: SR+FG 主流程拼装(Stage 1 → 2 → 4 → 5 → 6 → 7) [deep]

Wave 7 (After Wave 6 - Docs, 5 parallel):
├── 39: README.md v2 [writing]
├── 40: docs/build_guide.md [writing]
├── 41: docs/blender_export_guide.md [writing]
├── 42: docs/gui_guide.md + docs/cli_reference.md [writing]
└── 43: docs/user_manual.html(中英双语单文件) [writing]

Wave Final (After ALL implementation - 4 reviewer parallel,然后用户 okay):
├── F1: Plan 合规审计 [oracle]
├── F2: 代码质量 [unspecified-high]
├── F3: 真人手动 QA [unspecified-high + playwright]
└── F4: Scope fidelity check [deep]

Critical Path: 1 → (3 + 5) → (17 + 18 + 21 + 22 + 23) → (24 + 27) → 30 → 36 → 38 → F1-F4 → user okay
Max Concurrent: 9(Wave 1)
Parallel Speedup: ~65% vs sequential
```

### Dependency Matrix (abbreviated)

- **1**: blocks all → 1 是绝对前置
- **2-10**: 依赖 1;互不依赖
- **11-16**: 依赖 7(Blender 骨架)
- **17-23**: 依赖 3(CMake)+ 5(types);17 依赖 3;18 依赖 17;21-23 依赖 5
- **24**: 依赖 17, 18, 21, 22, 23
- ~~**25**~~: REMOVED(T25 CAS 已删,留 gap;无依赖关系)
- **26**: 依赖 17 + 5(is_hdr 字段)
- **27**: 依赖 17, 18, 21, 22, 23, 26(post-tonemap input)
- **28**: 依赖 17 + 26(EOTF 反变换共享 LUT 思路)
- **29**: 依赖 3(vcpkg ffmpeg)+ 5
- **30**: 依赖 8 + 17(plugin DLL link `dlss_engine_lib`)
- **31**: 依赖 30(transport 抽象)+ 全部 18-29(handler 调用引擎)
- **32-35**: 依赖 30(channel 接口稳定)
- **36**: 依赖 24 + 29 + 31(T25 已删,不再依赖)
- **37**: 依赖 26 + 27 + 28 + 29 + 31
- **38**: 依赖 36 + 37
- **39-43**: 依赖 38(整套管线 ready)
- **F1-F4**: 依赖 39-43

### Agent Dispatch Summary

- **Wave 0** (1 task):  T1 → `quick`
- **Wave 1** (9 tasks): T2/T4/T5/T6/T9 → `quick`;T3 → `unspecified-high`;T7/T8/T10 → `unspecified-low`
- **Wave 2** (6 tasks): T11 → `visual-engineering`;T12 → `ultrabrain`;T13 → `deep`;T14/T15/T16 → `unspecified-low`
- **Wave 3** (7 tasks): T17 → `ultrabrain`;T18/T23 → `deep`;T19/T20 → `unspecified-high`;T21/T22 → `unspecified-low`
- **Wave 4** (5 tasks,T25 已删): T24/T27 → `deep`;T26/T28 → `unspecified-low`;T29 → `unspecified-high`
- **Wave 5** (6 tasks): T30 → `deep`;T31 → `unspecified-high`;T32/T33/T34 → `visual-engineering`;T35 → `unspecified-low`
- **Wave 6** (3 tasks): T36/T37/T38 → `deep`
- **Wave 7** (5 tasks): T39-T43 → `writing`
- **Final** (4 tasks): F1 → `oracle`;F2/F3 → `unspecified-high`;F4 → `deep`

---

## TODOs

> Implementation + Test = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.
> **A task WITHOUT QA Scenarios is INCOMPLETE. No exceptions.**
> **FORMAT**: Task labels MUST use bare numbers: `1.`, `2.`, `3.` — NOT `T1.`, `Task 1.`, `Phase 1:`.
> Final Wave labels MUST use `F1.`, `F2.`, etc.

- [x] 1. v2 分支创建 + greenfield 清空

  **What to do**:
  - 在当前 repo 新建分支 `v2/main`(`git checkout -b v2/main`)
  - 删除根目录除以下之外的所有内容:`DLSS/`(submodule)、`.git/`、`.gitmodules`、`.omo/plans/dlss-compositor-v2.md`(plan 自身)、`.gitignore`(全新)
  - 创建空目录骨架:`src/`、`gui/`、`blender/`、`docs/`、`docs/dlss-refs/`、`tests/`、`tests/fixtures/`、`scripts/`、`.vscode/`、`.github/workflows/`
  - **Cherry-pick from v1**(per 用户决议:DLSS submodule + OpenEXR/通用 fixture 基础设施可带过来):
    - `docs/dlss-refs/*.md`(SR/FG/NIS doc 镜像 — 后续 task 必须查阅)
    - `tests/fixtures/camera.json` 和 `tests/fixtures/static_camera.json`(通用 camera fixture,per 用户 Q4)
    - `tests/fixtures/sequence/*.exr`(若兼容 Blender 5.1 Cycles 输出 + Halton base=8 + Vector 4ch 输出格式;**逐文件 review,不兼容的 T16 重生成**)
  - 提交一次 atomic commit

  **Must NOT do**:
  - 不要保留 v1 的 `src/` `gui/` `blender/` `docs/`(除 `dlss-refs/`)`scripts/` `tools/` `tests/`(除 cherry-pick 范围)`assets/` `.sisyphus/`
  - 不要保留 `package-release.ps1` 等 v1 脚本(v2 重写)
  - 不要复制 v1 的 README / AGENTS.md / 任何 .cpp/.h/.ts/.tsx/.py 文件
  - 不要把不兼容 v2 数据格式的 fixture EXR 带过去(若不确定,先不带;T16 会重生成)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 文件操作 + git 命令,无逻辑复杂度
  - **Skills**: [`git-master`]
    - `git-master`: 分支创建 + 大批量删除 + atomic commit 所必需

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential(Wave 0 唯一任务)
  - **Blocks**: 2-43 全部
  - **Blocked By**: None

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` v1 路径(cherry-pick 源)
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` v1 路径
  - `tests/fixtures/camera.json` v1 路径(cherry-pick 通用 fixture)
  - **WHY**:DLSS doc 镜像后续每个 NGX 任务都要 grep 引用;通用 camera.json 节省 Wave 1 的 fixture 生成时间

  **Acceptance Criteria**:
  - [ ] `git branch --show-current` → `v2/main`
  - [ ] `git ls-files | wc -l` → 仅 `DLSS/` 内文件 + `.gitmodules` + `.gitignore` + `.omo/plans/dlss-compositor-v2.md` + `docs/dlss-refs/*.md` + 通用 fixtures(camera.json/static_camera.json,可选若评估兼容则带 `tests/fixtures/sequence/*.exr`)
  - [ ] `ls src gui blender docs tests scripts .vscode .github` → 全是空目录(除 `docs/dlss-refs` + `tests/fixtures` cherry-pick)

  **QA Scenarios**:
  ```
  Scenario: 分支与文件清空验证(happy path)
    Tool: Bash
    Preconditions: 在 v1 main(opus-rework)分支根目录
    Steps:
      1. git checkout -b v2/main 切到新分支
      2. 执行清空脚本(保留 DLSS/ + cherry-picked fixtures + plan + dlss-refs)
      3. git add -A && git commit -m "chore: v2 greenfield branch initialized"
      4. 跑 git ls-files,验证只剩白名单文件 + cherry-picked fixtures
    Expected Result: 文件总数 ≤ 80(允许 dlss-refs md + 若干 fixture EXR;DLSS submodule 文件不算 git ls-files 范围内,因为是 submodule)
    Failure Indicators: 出现 src/main.cpp / gui/electron/main.ts 等 v1 残留 / 不属白名单的 v1 src 文件
    Evidence: .omo/evidence/task-1-clean-tree.txt(git ls-files 输出)

  Scenario: 不能保留 v1 关键文件(failure path)
    Tool: Bash
    Steps:
      1. git ls-files | grep -E "src/main\.cpp|gui/electron|blender/dlss_compositor_aov" → 应 0 匹配
    Expected Result: grep 退出码 1(无匹配)
    Evidence: .omo/evidence/task-1-no-v1-residue.txt

  Scenario: Cherry-picked fixtures 兼容性(per 用户 EXR 决议)
    Tool: Bash + python
    Steps:
      1. for each cherry-picked tests/fixtures/sequence/*.exr:
         a. python script 用 OpenEXR 读 channels
         b. 验证含 Combined RGBA + Z + Vector 4ch
         c. 验证 jitter (若 metadata 有)是 Halton(2,3) base=8 序列
      2. 不兼容的列出 + 删除(T16 重生成)
    Expected Result: 留下的 fixtures 全部兼容 v2 数据格式
    Evidence: .omo/evidence/task-1-fixture-compat.txt
  ```

  **Commit**: YES (single)
  - Message: `chore: v2 greenfield branch initialized`
  - Files: 大量删除 + 空目录骨架 + cherry-picked dlss-refs 与通用 fixtures

- [x] 2. DLSS submodule 升级 v310.5.3 → v310.6.0

  **What to do**:
  - `cd DLSS && git fetch origin && git checkout v310.6.0 && cd ..`
  - `git add DLSS && git commit -m "build(dlss): upgrade submodule to v310.6.0"`
  - 写一个 `docs/dlss-refs/CHANGELOG-310.6.0.md` 记录变化:`pNoPostProcessingColor` 删除、`ResourceAlwaysProvided/NeverProvided_Flags` 新增、Preset E 状态变化
  - 验证 `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h` 不再含 `pNoPostProcessingColor`

  **Must NOT do**:
  - 不要在此 task 引用 SDK 头文件(那是后续 NGX wrapper task 的事)
  - 不要 vendor 任何 .lib/.dll(submodule 自带)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: submodule 切 tag + 写一个简短 changelog
  - **Skills**: [`git-master`]

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1(与 3-10 并行)
  - **Blocks**: 17-29(NGX wrapper / FG eval 都需要 v310.6.0 头文件)
  - **Blocked By**: 1

  **References**:
  - `DLSS/include/nvsdk_ngx_defs_dlssg.h`(310.6.0)— 新 `ResourceFlags` enum
  - `DLSS/include/nvsdk_ngx_helpers_dlssg_vk.h`(310.6.0)— 新字段
  - **Pre-implementation verification**(plan generation 期已通过 `git -C DLSS show v310.6.0:include/...` 直接核验):
    - `git -C DLSS show v310.6.0:include/nvsdk_ngx_helpers_dlssg_vk.h | grep "NoPostProcessing"` → **0 行**(确认 v310.6.0 已删 `pNoPostProcessingColor`)
    - `git -C DLSS show v310.6.0:include/nvsdk_ngx_defs_dlssg.h | grep "ResourceAlwaysProvided\|ResourceNeverProvided"` → **2 行匹配**(确认新增 `_Flags` 宏)
    - upstream commit `cb017647ba9639759cf878ce8a2926ec5e0149cd` = `refs/tags/v310.6.0`
    - 这些验证已在 plan 生成阶段做过,T2 implementer 须在 submodule 切到 v310.6.0 后**重新跑这些 grep**确认本地副本与 upstream tag 一致
  - **WHY**:确认升级带来的字段变化,写入 changelog 供后续任务引用;upstream 已直接核验避免 plan 后置实现走错 SDK

  **Acceptance Criteria**:
  - [ ] `git -C DLSS describe --tags` → `v310.6.0`
  - [ ] `grep -r "pNoPostProcessingColor" DLSS/include/` → 0 匹配
  - [ ] `grep -r "ResourceAlwaysProvided" DLSS/include/` → ≥1 匹配
  - [ ] `docs/dlss-refs/CHANGELOG-310.6.0.md` 存在,列至少 3 条变化

  **QA Scenarios**:
  ```
  Scenario: Submodule tag 与 API 变化双验证(happy)
    Tool: Bash
    Preconditions: task 1 已完成,DLSS/ submodule 在 v310.5.3
    Steps:
      1. cd DLSS && git checkout v310.6.0
      2. git -C DLSS describe --tags → 应输出 v310.6.0
      3. grep -c "pNoPostProcessingColor" DLSS/include/nvsdk_ngx_*.h → 0
      4. grep -c "ResourceAlwaysProvided_Flags" DLSS/include/nvsdk_ngx_defs_dlssg.h → ≥1
    Expected Result: 4 项断言全过
    Evidence: .omo/evidence/task-2-submodule-upgrade.txt(git describe + grep counts)

  Scenario: Tag 不存在 fallback(failure)
    Tool: Bash
    Preconditions: 假设网络隔离,无法 fetch
    Steps:
      1. cd DLSS && git fetch origin v310.6.0 → 网络失败
      2. agent 必须 报错退出,标记 task FAILED,而不是 silently 留在 v310.5.3
    Expected Result: agent 显式输出 "DLSS v310.6.0 fetch failed, aborting"
    Evidence: .omo/evidence/task-2-fetch-failure.txt
  ```

  **Commit**: YES
  - Message: `build(dlss): upgrade submodule to v310.6.0`
  - Files: `DLSS`(submodule pointer)+ `docs/dlss-refs/CHANGELOG-310.6.0.md`

- [x] 3. 根 CMakeLists.txt + vcpkg 依赖配置

  **What to do**:
  - 写根 `CMakeLists.txt`:
    - C++20、MSVC `/W4 /WX`、CMake 3.20+
    - **双 build target**(per Metis 决议):`add_library(dlss_engine_lib STATIC ...)` + `add_executable(dlss-compositor-cli ...)`(后者 link static lib + 含 main + JSON-RPC stdio loop)
    - DLSS submodule:`set(DLSS_SDK_ROOT DLSS)` 强制要求 + FATAL_ERROR if 缺失
    - 依赖管理:**`vcpkg`** + `CMakePresets.json`,triplet `x64-windows-static-md`
      - `nlohmann_json`、`OpenEXR`、`Catch2`、`spdlog`、`volk`、`vk-bootstrap`(可选)、`vulkan-memory-allocator`、`ffmpeg`(prores_ks 编码器)
    - Glslc shader 编译流水线(vulkan-sdk 自带)→ `.spv`
    - DLSS DLLs 自动 copy 到输出目录:`add_custom_command(COPY DLSS/lib/Windows_x86_64/rel/*.dll)`
    - Debug build 链 `nvsdk_ngx_d_dbg.lib`,Release 链 `nvsdk_ngx_d.lib`
  - 写 `vcpkg.json`(manifest mode)+ `vcpkg-configuration.json`
  - 写 `CMakePresets.json`(`x64-windows-msvc-release` / `-debug`)
  - 写一个 `src/main_smoke.cpp` 临时入口(`int main() { return 0; }`)证明能 build,后续 task 17 替换

  **Must NOT do**:
  - 不要硬编码 vcpkg 安装路径(用 `VCPKG_ROOT` env)
  - 不要在此 task 写任何业务逻辑代码(纯 build 系统)
  - 不要引入 ImGui / GLFW / swapchain 相关依赖(compute-only)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: vcpkg manifest + CMake + DLL 拷贝 + glslc 流水线属于复杂构建系统配置
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 17, 19, 20, 21, 22, 23, 26, 28, 29(所有 C++ 实现 task,T25 已删)
  - **Blocked By**: 1

  **References**:
  - v1 `CMakeLists.txt`(可参考 DLSS submodule 链接、glslc 流水线模式),但 v2 完全重写
  - vcpkg manifest mode docs: https://learn.microsoft.com/en-us/vcpkg/users/manifests
  - DLSS submodule `lib/Windows_x86_64/rel/*.dll` 与 `*_dbg.lib` 路径
  - **WHY**:v1 的 CMake 是 FetchContent 模式,v2 改 vcpkg 更标准化;DLL/lib 路径必须从 submodule 内拷,不能依赖系统 PATH

  **Acceptance Criteria**:
  - [ ] `cmake -B build --preset x64-windows-msvc-release` 配置成功
  - [ ] `cmake --build build --config Release` 0 warning 0 error(MSVC `/W4 /WX`)
  - [ ] `build/Release/dlss-compositor-cli.exe` 存在,运行 → exit 0
  - [ ] `build/Release/nvngx_dlss.dll` 存在(自动拷贝)
  - [ ] `build/Release/nvngx_dlssg.dll` 存在
  - [ ] `cmake --build build --target dlss_engine_lib` → 产生 `dlss_engine_lib.lib`

  **QA Scenarios**:
  ```
  Scenario: 全新克隆 + submodule + build(happy path)
    Tool: Bash + cmake
    Preconditions: 全新 CI 容器,VS2022 已装,vcpkg 已 bootstrap
    Steps:
      1. git clone --recurse-submodules <repo> && cd <repo>
      2. git checkout v2/main
      3. cmake -B build --preset x64-windows-msvc-release
      4. cmake --build build --config Release --parallel
      5. ls build/Release/ → 验证 cli.exe + 2 dll 都在
      6. ./build/Release/dlss-compositor-cli.exe → exit code 0
    Expected Result: build 完整通过,可执行文件可启动
    Evidence: .omo/evidence/task-3-full-build.log

  Scenario: DLSS_SDK_ROOT 未设(failure)
    Tool: Bash
    Steps:
      1. cmake -B build_bad -DDLSS_SDK_ROOT="" → 应 FATAL_ERROR
    Expected Result: cmake 退出非 0,stderr 含 "DLSS_SDK_ROOT must be set"
    Evidence: .omo/evidence/task-3-missing-sdk-root.log

  Scenario: vcpkg 未 bootstrap(failure)
    Tool: Bash
    Steps:
      1. unset VCPKG_ROOT
      2. cmake -B build_bad → 应失败
    Expected Result: cmake 报 vcpkg 缺失
    Evidence: .omo/evidence/task-3-no-vcpkg.log
  ```

  **Commit**: YES
  - Message: `build: introduce vcpkg + dual-target CMake (engine lib + cli exe)`
  - Files: `CMakeLists.txt`、`vcpkg.json`、`vcpkg-configuration.json`、`CMakePresets.json`、`src/main_smoke.cpp`

- [x] 4. spdlog 集成(stderr + 文件 sink)

  **What to do**:
  - 写 `src/core/log.h` + `log.cpp`:封装 spdlog 单例 logger
  - 配置:
    - 默认输出 `stderr`(不污染 stdout — stdout 留给 RPC/MethodChannel)
    - 同时输出到文件 `dlss-compositor.log`(rotating,5MB × 3 files)
    - Log level 通过 env `DLSS_COMPOSITOR_LOG=trace|debug|info|warn|error|off` 控制,默认 `info`
    - format pattern: `[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v`
  - 暴露 macros: `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` / `LOG_WARN` / `LOG_ERROR`
  - 写 1 个 Catch2 单测验证:写 INFO 后能从文件读到对应行;stdout 干净(无任何 spdlog 输出)
  - 把 `dlss_engine_lib` 链入 spdlog

  **Must NOT do**:
  - **不要默认输出到 stdout**(违反 stdout-reserved-for-RPC 原则)
  - 不要把 logger 暴露成全局变量,通过 `Log::instance()` 单例
  - 不要在 header 里 `#include <spdlog/spdlog.h>`(防 ABI 漏出),只包 forward decl

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 单文件 wrapper + 1 个测试,vcpkg 已带 spdlog
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 17-29(后续 C++ task 都要 LOG_*)
  - **Blocked By**: 1, 3(CMake 必须 ready)

  **References**:
  - spdlog docs: https://github.com/gabime/spdlog
  - DLSS Compositor v2 决策:log → stderr + 文件,stdout 留 RPC
  - **WHY**:严格分离 stdout(数据协议)与 stderr(诊断)是 v2 与 v1 关键差异之一;v1 把 NDJSON 和 log 混在 stdout 导致 GUI 解析痛苦

  **Acceptance Criteria**:
  - [ ] `src/core/log.h` + `src/core/log.cpp` 存在
  - [ ] Catch2 测试 `tests/test_log.cpp` 跑过:写 LOG_INFO("hello") 后 stderr 含 "hello",stdout 空,文件含 "hello"
  - [ ] `DLSS_COMPOSITOR_LOG=off` env → 全部 sink 沉默
  - [ ] `dlss_engine_lib` link 后能正常用

  **QA Scenarios**:
  ```
  Scenario: Stderr + 文件双 sink + stdout 干净(happy)
    Tool: Bash
    Preconditions: build 已成功
    Steps:
      1. echo 'int main(){LOG_INFO("test-msg-42"); return 0;}' > tmp_main.cpp
      2. 编译并跑:./tmp_test 2>stderr.log >stdout.log
      3. grep -c "test-msg-42" stderr.log → 1
      4. wc -c stdout.log → 0(stdout 必须空)
      5. grep -c "test-msg-42" dlss-compositor.log → 1
    Expected Result: 4 项断言全过
    Evidence: .omo/evidence/task-4-spdlog-stderr-stdout.txt

  Scenario: Log level off 静默(happy edge)
    Tool: Bash
    Steps:
      1. DLSS_COMPOSITOR_LOG=off ./tmp_test 2>stderr.log
      2. wc -c stderr.log → 0
    Evidence: .omo/evidence/task-4-spdlog-off.txt

  Scenario: 误输出到 stdout 检测(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'std::cout << $$$' src/  → 应 0 匹配
      2. ast_grep --lang cpp 'printf($$$)' src/        → 应 0 匹配
      3. ast_grep --lang cpp 'fprintf(stdout, $$$)' src/ → 应 0 匹配
    Expected Result: src/ 下无任何 stdout 写入痕迹
    Evidence: .omo/evidence/task-4-no-stdout-pollution.txt
  ```

  **Commit**: YES
  - Message: `feat(core): spdlog logger to stderr + rotating file (stdout reserved for RPC)`
  - Files: `src/core/log.h`、`src/core/log.cpp`、`tests/test_log.cpp`、`CMakeLists.txt`(链 spdlog)

- [x] 5. core types + dlssconfig.schema.json(single source of truth)

  **What to do**:
  - 写 `src/core/types.h`:
    - `Resolution { uint32_t width; uint32_t height; }`
    - `enum class QualityMode { DLAA, UltraQuality, Quality, Balanced, Performance, UltraPerformance }`(顺序与 DLSS doc 一致)
    - `enum class DlssPreset { Auto, J, K, L, M }`
    - `struct JitterPair { float x; float y; }` // pixel space [-0.5, +0.5]
    - `struct CameraData { float matrix_world[4][4]; float projection[4][4]; float fov; float aspect_ratio; float near_clip; float far_clip; JitterPair jitter; }`
    - `struct DlssConfig { int version; Resolution output_resolution; Resolution render_resolution; QualityMode quality; std::string display_device; std::string view_transform; bool is_hdr; int fps; std::string filename_pattern; int jitter_phases; std::map<std::string, CameraData> frames; }`
    - `enum class OutputFormat { ExrLinear, Png16, ProRes422HQ, /* not implemented in v2 - extension hooks: */ Jpeg, H264Mp4, H265Mp4, Av1Mp4, Avif, ProRes4444XQ, DNxHR }`(per T29 + 用户 round 2 决议:LDR 用 PNG,扩展项不注册到 EncoderRegistry)
    - 静态常量 `DLSS_RATIO[QualityMode]`:`{1.0, 1.0, 1.5, 1.724, 2.0, 3.0}`(per DLSS §3.7.1.1)
    - 静态常量 `JITTER_PHASES[QualityMode]`:`{8, 8, 18, 24, 32, 72}`(per §3.7.1.1 minimum table)
    - 静态常量 `DEFAULT_PRESET[QualityMode]`:`{K, K, K, K, M, L}`(per §3.12 NVIDIA 推荐)
  - 写 `src/core/dlssconfig.schema.json`:**single source of truth** for Blender writer / CLI reader / Flutter reader
    - JSON Schema Draft 2020-12
    - 必填 fields:`version`(integer, const 1), `output_resolution`, `render_resolution`, `quality_mode`(enum), `display_device`, `view_transform`, `is_hdr`(bool), `fps`(integer), `filename_pattern`(regex string), `jitter_phases`(integer)
    - `frames` object,key 是 zero-padded 帧号 string,value 是 per-frame camera object
    - per-frame fields:`matrix_world`(4×4 array), `projection`(4×4 array), `fov`(float), `aspect_ratio`(float), `near_clip`(float), `far_clip`(float), `jitter_x`(float, range [-0.5, +0.5]), `jitter_y`(float, range [-0.5, +0.5])
    - 注释字段:`schema 版本由 v2 plan task 5 定义,后续修改必须 bump version`
  - 写 1 个 Catch2 测试:验证 `DLSS_RATIO` / `JITTER_PHASES` / `DEFAULT_PRESET` 与 SR doc 表完全一致(防回归)
  - 在 `dlss_engine_lib` 中 `target_include_directories(... src/)` 暴露 `core/`

  **Must NOT do**:
  - **不要在 schema 里加 `cut_after` 字段**(per Metis Q3 决议,camera cut 由 CLI 端推导)
  - 不要在 `JITTER_PHASES` 用 base=1(违反 §3.7.1.1)
  - 不要把 schema 散落在多个文件,所有 layer 都引用同一份 `core/dlssconfig.schema.json`
  - 不要给 `OutputFormat` 添加未在 T29/T33 列出的项(允许:ExrLinear / Png16 / ProRes422HQ + **7 个** future 标注的扩展项 Jpeg/H264Mp4/H265Mp4/Av1Mp4/Avif/ProRes4444XQ/DNxHR;**不允许**:WebP/HDR PNG/TIFF 等未规划的格式)
  - **不要让 EncoderRegistry 注册扩展项**(Jpeg/H264Mp4/H265Mp4/Av1Mp4/Avif/ProRes4444XQ/DNxHR 仅是 enum 占位 + docs 提及,**v2 实现期不注册**)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: 类型定义 + JSON schema + 1 个常量表测试,逻辑清晰
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 6, 11, 13, 15, 21, 24, 27, 31, 32, 33, 34
  - **Blocked By**: 1, 3

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.7.1.1(scaling ratios + minimum phase counts table 行 980-984)
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.12(per-quality preset 推荐 K/L/M)
  - JSON Schema 2020-12 spec: https://json-schema.org/draft/2020-12/json-schema-core.html
  - **WHY**:`DLSS_RATIO` / `JITTER_PHASES` / `DEFAULT_PRESET` 三张表是整个项目的 DLSS 一致性基石,T11/T12/T15 写入,T6/T24 读取必须用同一份。Schema-as-code 能在编译期 catch 不一致

  **Acceptance Criteria**:
  - [ ] `src/core/types.h` 编译通过
  - [ ] `src/core/dlssconfig.schema.json` 是合法 JSON Schema 2020-12(用 ajv 或 jsonschema 工具校验)
  - [ ] Catch2 测试 `test_core_constants.cpp`:断言 `DLSS_RATIO[Performance] == 2.0f` 等 5 项,`JITTER_PHASES[UltraPerformance] == 72` 等 5 项
  - [ ] `grep -c "cut_after" src/core/dlssconfig.schema.json` → 0(显式不含)
  - [ ] `grep -E "WebP|webp|tiff|TIFF|HdrPng" src/core/types.h` → 0(OutputFormat 不含未规划格式;允许 ExrLinear/Png16/ProRes422HQ + 7 个标 future 的 Jpeg/H264Mp4/H265Mp4/Av1Mp4/Avif/ProRes4444XQ/DNxHR 占位)

  **QA Scenarios**:
  ```
  Scenario: 常量表与 DLSS doc 一致(happy)
    Tool: Bash + Catch2
    Steps:
      1. 跑 ./build/tests/test_core_constants
      2. 验证所有 5 个 quality mode 的 DLSS_RATIO / JITTER_PHASES / DEFAULT_PRESET 匹配
    Expected Result: 15+ 断言全过
    Evidence: .omo/evidence/task-5-constants-table.txt(测试输出)

  Scenario: Schema validates a valid sample(happy)
    Tool: Python + jsonschema
    Steps:
      1. 用 python -m jsonschema -i tests/fixtures/sample-dlssconfig.json src/core/dlssconfig.schema.json
      2. exit code 0
    Evidence: .omo/evidence/task-5-schema-validate-pass.txt

  Scenario: Schema rejects cut_after / DNxHR(failure path)
    Tool: Python + jsonschema
    Steps:
      1. 构造 bad sample {"version":1, "cut_after": true, ...}
      2. python -m jsonschema → 应失败(additionalProperties: false)
      3. 构造 bad sample {"output_format": "dnxhr"} → 应失败(enum 不含)
    Expected Result: 两个 bad sample 都被拒
    Evidence: .omo/evidence/task-5-schema-rejects-bad.txt
  ```

  **Commit**: YES
  - Message: `feat(core): types + dlssconfig.schema.json single source of truth`
  - Files: `src/core/types.h`、`src/core/dlssconfig.schema.json`、`tests/test_core_constants.cpp`、`tests/fixtures/sample-dlssconfig.json`

- [x] 6. core/ dlssconfig 加载 + validate(C++ side)

  **What to do**:
  - 写 `src/core/dlssconfig.h` + `dlssconfig.cpp`:
    - `class DlssConfigLoader` 提供 `static std::expected<DlssConfig, LoadError> Load(std::filesystem::path)`
    - 内部用 `nlohmann::json` 解析,然后 schema 验证(基于 `core/dlssconfig.schema.json` 的 `nlohmann::json_schema_validator` library 或自写最小校验器)
    - **关键 invariants 验证**(超出 JSON schema 能力的逻辑校验):
      - `version == 1`,否则报错
      - 所有 per-frame `jitter_x/y` 在 `[-0.5, +0.5]`(内部 clamp 到 `[-0.499, +0.499]` 防越界 NaN)
      - `render_resolution.width <= output_resolution.width`(同 height)
      - `render_resolution` 与 `quality_mode` 配对推导值匹配(`output / DLSS_RATIO[quality]` 的整数,允许 ±1 误差)
      - `frames` 字典 key 与 `filename_pattern` regex 匹配
    - `LoadError` 包含 `code`(`enum: BadJson, SchemaFail, VersionMismatch, ResolutionInconsistent, FrameKeyMismatch, FileNotFound`,**6 codes**;jitter 越界不报错走 clamp 路径,不计入此 enum)+ `message`(human readable)+ `path`(JSON pointer 指向出错位置)
  - 写 Catch2 测试:5 个 happy + 6 个 each-error path
  - 用真实样例 `tests/fixtures/sample-dlssconfig.json` 跑通

  **Must NOT do**:
  - 不要 silently 修改用户提供的字段值(只 clamp jitter,记录 warning,但其他都拒绝)
  - 不要把 LoadError 设为 string 拼接(必须结构化,GUI/RPC 要解析)
  - 不要 throw 异常(用 `std::expected` 或 `tl::expected` 风格)
  - **不要在此 task 实现 camera cut 检测**(那是 T24/T36 的事,基于 matrix discontinuity)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: JSON 解析 + validation + 错误模型,标准模式
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 24, 27, 31, 36, 37, 38(管线启动需先加载 config)
  - **Blocked By**: 5

  **References**:
  - `src/core/dlssconfig.schema.json`(T5 产出,single source of truth)
  - `nlohmann::json` docs: https://json.nlohmann.me/
  - `nlohmann/json-schema-validator` lib(可选)
  - **WHY**:dlssconfig 是三层架构的契约;严格 validation 在加载点拒绝坏数据,避免管线深处 NaN 崩溃

  **Acceptance Criteria**:
  - [ ] `DlssConfigLoader::Load(path)` 返回 `std::expected<DlssConfig, LoadError>`
  - [ ] `tests/fixtures/sample-dlssconfig.json` 加载成功
  - [ ] 6 个 error path 测试全过(每个 LoadError code 一条)
  - [ ] `grep -E "throw " src/core/dlssconfig.cpp` → 0(no exceptions)
  - [ ] **Jitter clamp 策略**:`(-0.499, +0.499)` 内值正常通过;在容忍区间`[-0.499 ± 1e-3]`边缘的值 → silent clamp(无 warn);明显出界(|x| > 0.5)→ silent clamp 到 `±0.499` + LOG_WARN(per Metis 决议)。**完全不 reject**(防止 Blender 浮点边缘导致整段渲染白干)

  **QA Scenarios**:
  ```
  Scenario: 加载有效 dlssconfig.json(happy)
    Tool: Catch2
    Steps:
      1. DlssConfigLoader::Load("tests/fixtures/sample-dlssconfig.json")
      2. 断言:返回 DlssConfig,version == 1,fps == 30,frames.size() == 60
    Evidence: .omo/evidence/task-6-load-valid.txt

  Scenario: 拒绝 schema 不合法 JSON(failure)
    Tool: Catch2
    Steps:
      1. 构造 {"version": 2, ...} → 应返回 LoadError{VersionMismatch}
      2. 构造缺 `render_resolution` → 应返回 LoadError{SchemaFail}
      3. 构造 render_resolution.width > output_resolution.width → ResolutionInconsistent
      4. 构造 frames key "9999" 但 filename_pattern 不匹配 → FrameKeyMismatch
      5. 文件不存在 → FileNotFound
    Expected Result: 5 个 LoadError code 各一次精确触发(jitter 越界单独走 clamp 路径,不在此 reject 列表)
    Evidence: .omo/evidence/task-6-error-paths.txt

  Scenario: Jitter clamp 策略(edge case,never reject)
    Tool: Catch2
    Steps:
      1. 构造 jitter_x = -0.4995(在容忍区间)→ silent clamp 到 -0.499,no warn
      2. 构造 jitter_x = -0.4999999(浮点边缘)→ clamp 到 -0.499 + LOG_WARN
      3. 构造 jitter_x = -0.55(明显超界)→ silent clamp 到 -0.499 + LOG_WARN(**不 reject**,per Metis 决议防边缘 NaN)
      4. 构造 jitter_x = +1.0(极端值)→ silent clamp 到 +0.499 + LOG_WARN
    Evidence: .omo/evidence/task-6-jitter-clamp.txt
  ```

  **Commit**: YES
  - Message: `feat(core): dlssconfig loader with schema + invariant validation`
  - Files: `src/core/dlssconfig.h`、`src/core/dlssconfig.cpp`、`tests/test_dlssconfig_loader.cpp`、`tests/fixtures/dlssconfig-bad-*.json`(6 个 error fixture)

- [x] 7. Blender 插件 v2 骨架(__init__ / manifest / AOV register)

  **What to do**:
  - 创建 `blender/dlss_compositor/` 目录:
    - `__init__.py`:`bl_info` 字典(name, version, blender 5.1+, category)+ `register()` / `unregister()` 注册各 operator + panel + property group + handler
    - `blender_manifest.toml`:Blender 5.1+ extension manifest(id, version, name, blender_version_min = "5.1.0")
    - `aov_register.py`:封装 Combined / Depth / Vector pass 的 view layer 设置(`view_layer.use_pass_z = True`、`view_layer.use_pass_vector = True`、激活 Combined)
    - `properties.py`:`DLSSCompositorProperties` PropertyGroup(`enabled` bool default False、`quality_mode` enum、`output_dir` DirPath)
    - **不要加** `output_width` / `output_height` IntProperty(冗余于 `scene.render.resolution_x/y`,per T11 决议)
    - **不要加** `resolution_percentage` 字段(由 handler save/override/restore 透明处理,per T14 决议)
    - `panel.py`:`DLSS_PT_Main` 在 `Render Properties` 注册一个空面板(后续 T11 填 UI)
  - 写 `blender/tests/test_register.py` pytest:
    - 用 `bpy` mock(或真 Blender headless `blender --background --python`)验证 register 成功 + properties 出现在 scene + panel 注册到 RENDER_PT_*
    - 验证 unregister 干净(无 dangling reference)
  - 写 `blender/INSTALL.md`:开发模式安装步骤

  **Must NOT do**:
  - 不要在此 task 写 Halton jitter / handler / dlssconfig 写入(那是 T12/T14/T15)
  - 不要写 UI 控件(T11)
  - 不要使用 deprecated `bl_info` 风格 add-on(必须用 5.1+ extension manifest)
  - 不要往 properties 里加 `cut_after` 或 `scale` 字段(per 用户决议,scale 由 quality 推导)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Blender extension scaffolding + register/unregister 是模板化工作
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 11, 12, 13, 14, 15, 16
  - **Blocked By**: 1

  **References**:
  - Blender 5.1+ Extension docs: https://docs.blender.org/manual/en/latest/extensions/index.html
  - `blender_manifest.toml` 规范
  - Blender Cycles AOV setup(`view_layer.use_pass_*`)
  - **WHY**:v1 用旧 `bl_info` add-on 模式,v2 必须用 5.1+ extension(更稳定 install + 商店分发支持)

  **Acceptance Criteria**:
  - [ ] `blender/dlss_compositor/__init__.py` 含 `register` / `unregister` 函数
  - [ ] `blender_manifest.toml` 通过 `python -m blender_extensions validate`(或等效校验)
  - [ ] `pytest blender/tests/test_register.py` 全过(register/unregister 不抛错,property group 注册成功)
  - [ ] 在真 Blender 5.1+ 中安装该扩展 → 出现在 Edit > Preferences > Get Extensions

  **QA Scenarios**:
  ```
  Scenario: pytest mock 验证 register 流程(happy)
    Tool: Bash + pytest
    Steps:
      1. cd blender && python -m pytest tests/test_register.py -v
      2. 断言:register() 不抛错;scene.dlss_compositor PropertyGroup 出现;DLSS_PT_Main panel 注册成功
      3. unregister() 清理,property group 消失
    Evidence: .omo/evidence/task-7-register-pytest.txt

  Scenario: 真 Blender headless 安装(integration)
    Tool: Bash
    Steps:
      1. zip -r dlss_compositor.zip blender/dlss_compositor/
      2. blender --background --python-expr "import bpy; bpy.ops.preferences.addon_install(filepath='dlss_compositor.zip'); bpy.ops.preferences.addon_enable(module='dlss_compositor')"
      3. 验证 stderr 无 ERROR
    Evidence: .omo/evidence/task-7-blender-install.log

  Scenario: 不该有的 properties 检测(failure prevention)
    Tool: grep
    Steps:
      1. grep -rE "cut_after|propose_scale|custom_scale" blender/dlss_compositor/  → 应 0 匹配
    Evidence: .omo/evidence/task-7-no-forbidden-props.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): v2 extension scaffold (manifest 5.1+, register, AOV passes)`
  - Files: `blender/dlss_compositor/__init__.py`、`blender_manifest.toml`、`aov_register.py`、`properties.py`、`panel.py`、`blender/tests/test_register.py`、`blender/INSTALL.md`

- [x] 8. Flutter app + plugin_ffi 脚手架(Win32 file dialog FFI)

  **What to do**:
  - `cd gui && flutter create --template=app --platforms=windows dlss_compositor_gui`
  - 改 `pubspec.yaml`:Flutter 3.22+,Dart 3.4+
  - 添加依赖:`ffi`、`win32`、`shared_preferences`、`provider`(状态管理)
  - 创建 `gui/dlss_compositor_plugin/`(用 `flutter create --template=plugin_ffi --platforms=windows dlss_compositor_plugin`)
  - 在 `gui/lib/util/win_file_dialog.dart` 实现 `selectFile({required String filterDescription, required String filterPattern})` 与 `selectDirectory()`,基于 `package:win32` `IFileOpenDialog`(参考用户提到的 `d:\FlutterProjects\shader_validation_utils\lib\utils\win_api.dart` 模式)
  - 写 1 个简单的 `lib/main.dart`:Material app + 单个按钮"Open dlssconfig.json" → 弹出 Win32 file dialog → 选中后在 stdout 打印路径(临时 log 验证 wiring)
  - 写 `gui/test/win_file_dialog_test.dart` widget test(mock dialog 返回固定路径,断言 UI 显示)
  - 写 `gui/integration_test/launch_test.dart`:`flutter test integration_test` 启动 app + 截图 + 验证 button 可见

  **Must NOT do**:
  - 不要在此 task 实现 plugin 的 native side(那是 T30)
  - 不要引入 Electron 任何残留依赖
  - 不要使用 `file_picker` package(用 Win32 FFI 更可控,符合用户偏好)
  - 不要支持 Linux/macOS platform(`flutter create --platforms=windows` only)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Flutter 脚手架 + 一个 FFI wrapper 函数
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 30, 32, 33, 34, 35
  - **Blocked By**: 1

  **References**:
  - `d:\FlutterProjects\shader_validation_utils\lib\utils\win_api.dart`(用户提供的 Win32 FileOpenDialog 参考实现)
  - Flutter desktop docs: https://docs.flutter.dev/platform-integration/desktop
  - Flutter plugin_ffi: https://docs.flutter.dev/platform-integration/platform-channels
  - `package:win32` 5.5+ docs
  - **WHY**:用户明确希望 Win32 原生 dialog(不要 file_picker package);plugin_ffi 模板会自动配 CMake 和 dart:ffi 绑定

  **Acceptance Criteria**:
  - [ ] `cd gui && flutter doctor -v`(Windows 平台 ok)
  - [ ] `flutter analyze`(0 issue)
  - [ ] `flutter test`(widget test 全过)
  - [ ] `flutter test integration_test/launch_test.dart`(app 启动 + 截图 evidence)
  - [ ] `flutter build windows --release`(产生 `build/windows/x64/runner/Release/dlss_compositor_gui.exe`)
  - [ ] 双击 exe 启动:窗口出现 + button 可见 + 点击弹出系统 file dialog(手动 / playwright 暂不验证 dialog 交互,因为 Win32 dialog 跨进程)

  **QA Scenarios**:
  ```
  Scenario: Flutter app 构建 + 启动(happy)
    Tool: Bash + flutter
    Steps:
      1. cd gui && flutter pub get
      2. flutter analyze → 0 issue
      3. flutter test → all pass
      4. flutter build windows --release → exe 产出
      5. 启动 exe,窗口标题 "DLSS Compositor v2"
    Evidence: .omo/evidence/task-8-flutter-build.log + screenshot

  Scenario: Win32 file dialog 单元测试(happy)
    Tool: Bash + flutter test
    Steps:
      1. mock IFileOpenDialog COM,返回 "C:\test\dlssconfig.json"
      2. 调 selectFile() → 应返回该 path
      3. 断言 widget 显示该 path
    Evidence: .omo/evidence/task-8-file-dialog-mock.txt

  Scenario: 不允许 Linux/macOS platform 残留(failure prevention)
    Tool: Bash
    Steps:
      1. ls gui/macos gui/linux 2>&1 → 应都不存在
    Evidence: .omo/evidence/task-8-windows-only.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): Flutter Windows app scaffold + Win32 FileOpenDialog FFI`
  - Files: `gui/pubspec.yaml`、`gui/lib/main.dart`、`gui/lib/util/win_file_dialog.dart`、`gui/test/win_file_dialog_test.dart`、`gui/integration_test/launch_test.dart`、`gui/dlss_compositor_plugin/*`(plugin_ffi 脚手架)

- [x] 9. .vscode/launch.json compound 双调试(Flutter Dart + C++ attach)

  **What to do**:
  - 写 `.vscode/launch.json` 三个 configurations + 一个 compound:
    1. **`Flutter (Debug)`**:`type=dart`,`request=launch`,`program=gui/lib/main.dart`,`cwd=${workspaceFolder}/gui`,`args=[--enable-vm-service]`
    2. **`C++ Attach to Flutter`**:`type=cppvsdbg`,`request=attach`,`processId=${command:pickProcess}`,`symbolSearchPath="${workspaceFolder}/gui/build/windows/x64/runner/Release;${workspaceFolder}/build/Release"`
    3. **`C++ Launch CLI`**:`type=cppvsdbg`,`request=launch`,`program=${workspaceFolder}/build/Release/dlss-compositor-cli.exe`,`args=["--config", "${workspaceFolder}/tests/fixtures/sample-dlssconfig.json"]`,`cwd=${workspaceFolder}`,`environment=[{"name":"DLSS_COMPOSITOR_LOG","value":"debug"}]`
    4. **Compound `Flutter + C++ (Plugin Debug)`**:启动 1 + 等 2 秒后 attach 2,实现双调试
  - 写 `.vscode/tasks.json`:`build-cli`(`cmake --build build --config Release`)+ `build-flutter`(`flutter build windows --release`),用作 launch 的 preLaunchTask
  - 写 `.vscode/settings.json`:推荐扩展(Dart-Code、ms-vscode.cpptools、CMake Tools)+ `cmake.configureOnOpen=false`(避免 vscode 自动跑导致 vcpkg 触发慢启动)
  - 写 `docs/debug_setup.md`(2 页):F5 双调试操作步骤、设置 PDB 路径、常见问题(symbol load 失败的处理)

  **Must NOT do**:
  - 不要硬编码 VS2022 安装路径(用 `${env:VSINSTALLDIR}` 等变量)
  - 不要让 launch.json 假设 build 已完成(必须 preLaunchTask 触发)
  - 不要把调试配置 commit 用户私人路径(用 `${workspaceFolder}`)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: launch.json + tasks.json 配置文件,模板化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 无技术依赖,但所有后续 task 的开发体验受益
  - **Blocked By**: 1

  **References**:
  - VSCode launch.json compound docs: https://code.visualstudio.com/docs/editor/debugging#_compound-launch-configurations
  - Dart-Code extension docs
  - cppvsdbg attach pattern
  - `bg_36a24283` 调研报告 §E(Tim Sneath blog 模式)
  - **WHY**:用户明确要求 VSCode F5 调试体验;compound launch 是研究确认的可行模式;preLaunchTask 防止常见的"忘了 build"问题

  **Acceptance Criteria**:
  - [ ] `.vscode/launch.json` 含 3 个 configurations + 1 compound(compound 引用其中 2 个 individual configs)
  - [ ] 在 VSCode 中按 F5 选 `Flutter + C++` 能启动两个调试 session
  - [ ] 在 Dart 代码设断点 + C++ 代码设断点 → 都命中(用一个调用 plugin 的简单 demo,即使后续 task 30 才完整实现)
  - [ ] `docs/debug_setup.md` ≥ 2 页(setup + troubleshooting)

  **QA Scenarios**:
  ```
  Scenario: launch.json 结构合法(happy)
    Tool: Bash + jq
    Steps:
      1. jq '.configurations | length' .vscode/launch.json → 3
      2. jq '.compounds | length' .vscode/launch.json → 1
      3. jq '.compounds[0].configurations | length' .vscode/launch.json → 2
    Evidence: .omo/evidence/task-9-launch-json-shape.txt

  Scenario: VSCode 启动 compound 调试(integration, manual)
    Tool: VSCode launch via agent script(用 cscript / autohotkey / 类似工具自动按 F5 + 验证 debugger 状态)
    Steps:
      1. 打开 workspace,F5 选 "Flutter + C++ (Plugin Debug)"
      2. Flutter app 启动
      3. C++ debugger attach 成功(Variables 面板能看到符号)
    Evidence: .omo/evidence/task-9-compound-debug-screenshot.png

  Scenario: 缺 PDB 时 graceful 报错(failure)
    Tool: Manual
    Steps:
      1. 删除 build/Release/*.pdb
      2. F5 attach → VSCode 报 "符号未加载"提示
      3. docs/debug_setup.md 必须含此情况的解法
    Evidence: .omo/evidence/task-9-no-pdb-fallback.txt
  ```

  **Commit**: YES
  - Message: `chore(vscode): compound launch.json for Flutter + C++ dual debug`
  - Files: `.vscode/launch.json`、`.vscode/tasks.json`、`.vscode/settings.json`、`docs/debug_setup.md`

- [x] 10. GitHub Actions CI workflow

  **What to do**:
  - 写 `.github/workflows/build-test.yml`:
    - 触发:push 到 `v2/main` + PR 到 `v2/main`
    - 一个 job,`runs-on: windows-2022`
    - Steps:
      1. `actions/checkout@v4` with `submodules: recursive`
      2. 安装 vcpkg(`microsoft/setup-vcpkg-action@*`)
      3. 安装 Vulkan SDK 1.3+(下载 + silent install)
      4. 安装 Flutter 3.22+(`subosito/flutter-action@v2`)
      5. `cmake -B build --preset x64-windows-msvc-release`
      6. `cmake --build build --config Release --parallel`
      7. `ctest --test-dir build --config Release --output-on-failure`
      8. `cd gui && flutter pub get && flutter analyze && flutter test`
      9. (Blender pytest 暂跳过 — Blender 装在 CI 太重,改在 dev 本地)
      10. 上传 build artifacts(`actions/upload-artifact@v4`)
    - cache:vcpkg installed(`actions/cache@v4` keyed by `vcpkg.json` hash)
  - 写 `.github/workflows/release.yml`(tag `v*` 触发):`build-test.yml` 全过 + 打包 zip + 创建 GitHub Release
  - 写 `.github/CODEOWNERS`(占位,后续填)

  **Must NOT do**:
  - 不要在 CI 跑 GPU-dependent tests(NGX init / 真 SR/FG eval)— Windows runner 无 NVIDIA GPU,这些放本地 Agent QA
  - 不要安装 Blender(workflow 时间 > 10 分钟成本太高)
  - 不要 cache `build/` 目录(增量 build 在 CI 跨 commit 不可靠)
  - 不要让 PR 跳过任何 step

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: workflow YAML,多步骤但无算法复杂度
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1
  - **Blocks**: 无(CI 只是验证,不阻塞实现)
  - **Blocked By**: 1, 3

  **References**:
  - GitHub Actions Windows runner docs
  - `actions/cache@v4` for vcpkg
  - Vulkan SDK 1.4.309 silent install command(per v1 CI 经验)
  - **WHY**:CI 在 PR 阶段抓 build 回归;v1 已有 build-release.yml 但 v2 完全重写

  **Acceptance Criteria**:
  - [ ] `.github/workflows/build-test.yml` 在 push 后能成功跑通(在真 GitHub repo 测试)
  - [ ] 第 1 次跑约 ~15 min,缓存命中后 ~7 min
  - [ ] artifact 上传成功(zip 含 cli.exe + dll + flutter exe)
  - [ ] PR with intentional break(随便加 `int x = "string";` to src/) → CI fail with clear MSVC error
  - [ ] PR with intentional flutter break → analyze fail

  **QA Scenarios**:
  ```
  Scenario: 推送 trigger CI 成功(happy)
    Tool: GitHub Actions UI / gh CLI
    Steps:
      1. push 一个无意义改动到 v2/main
      2. gh run watch → 等到 success
      3. gh run download <id> → 验证 artifact 含 dlss-compositor-cli.exe
    Evidence: .omo/evidence/task-10-ci-pass.txt

  Scenario: 故意破坏 build 应失败(failure path)
    Tool: gh CLI
    Steps:
      1. push 一个含语法错的 cpp 到 PR
      2. gh run watch → fail
      3. 验证 stderr 含 MSVC C2xxx error code
    Evidence: .omo/evidence/task-10-ci-fail-on-bad-cpp.txt

  Scenario: 缓存命中验证(efficiency)
    Tool: gh CLI
    Steps:
      1. 第二次 push 后 gh run watch
      2. 验证 vcpkg cache hit 在 log 中
      3. 总耗时 < 10 分钟
    Evidence: .omo/evidence/task-10-ci-cache-hit.txt
  ```

  **Commit**: YES
  - Message: `ci: GitHub Actions Windows build + ctest + flutter test`
  - Files: `.github/workflows/build-test.yml`、`.github/workflows/release.yml`、`.github/CODEOWNERS`

- [x] 11. Blender Resolution + Quality Mode UI 面板

  **What to do**:
  - 在 `blender/dlss_compositor/panel.py` 完成 `DLSS_PT_Main` panel(在 `Render Properties` tab):
    - **Panel header checkbox(`enabled`)**:类似 Eevee Sampling 面板的 header toggle。`enabled=False` 时插件 handlers 全部 no-op,用户 `resolution_percentage` 完全不被碰,dlssconfig.json 不写。`enabled=True` 时插件介入(详见 T14 save/override/restore 流程)
    - 标题:`DLSS Compositor`(header 含 enable checkbox)
    - **Quality Mode 横排 toggle**(单选):用 `layout.row().prop(props, "quality_mode", expand=True)` 渲染 6 个按钮:`DLAA / UltraQuality / Quality / Balanced / Performance / UltraPerformance`(顺序 per DLSS doc)
    - **Output Directory**:DirPath 选择器(用 `bpy.types.Operator` 调用 file browser)
    - **Apply Settings 按钮**:**职责仅限 渲染前置准备**(不管 resolution_percentage,那是 T14 handler 的运行时职责):
      - 启用 view layer pass:`use_pass_z=True`、`use_pass_vector=True`、Combined 总开
      - 强制 `scene.cycles.use_denoising = True`
      - 在 compositor 中创建/更新 File Output 节点 → `output_dir/####.exr` (Combined) / `####.depth.exr` / `####.vector.exr`
      - 强制 File Output node `format.color_management='OVERRIDE'` + `view_settings.view_transform='Raw'`
      - **不**修改 `scene.render.resolution_percentage`(handler 在 render_pre 时做 save/override)
    - **可选 Status bar widget**(`bpy.types.STATUSBAR_HT_header.append`):仅 enabled=True 时显示 `DLSS: <Quality> → <pct>%`(供用户随时确认状态,无交互)
    - **UI 严格隐藏可计算 / 已存在的项**(per 用户决议):**不暴露**:
      - **输出 W/H**(Blender 标准 Render Properties 已有 `Resolution X/Y`,我们直接读)
      - **resolution_percentage**(handler 在 render_pre 时透明覆写)
      - Halton phase 数 / per-frame jitter X/Y / scale ratio / render_resolution / `resolution_percentage` 中间值
      - 所有这些由 `quality_mapping.py` + `jitter.py` 按 DLSS 公式自动计算,只在 dlssconfig.json 中写入
    - **Panel 仅 4 项可交互控件**:Enable checkbox(header)、Quality toggle、Output Dir、Apply Settings(配置 pass + compositor + denoise)
  - 写 `blender/dlss_compositor/operators.py`:
    - `DLSS_OT_ApplySettings`(Apply Settings 按钮 callback)
  - 写 `blender/dlss_compositor/quality_mapping.py`:
    - `DLSS_RATIO = {DLAA: 1.0, UltraQuality: 1.0, Quality: 1.5, Balanced: 1.724, Performance: 2.0, UltraPerformance: 3.0}`
    - `JITTER_PHASES = {DLAA: 8, UltraQuality: 8, Quality: 18, Balanced: 24, Performance: 32, UltraPerformance: 72}`
    - **必须与 `src/core/types.h` 的 `DLSS_RATIO` / `JITTER_PHASES` 完全一致**(后续 T16 pytest 加交叉验证)
  - 写 pytest:
    - Apply Settings 触发后:验证 view_layer.use_pass_z=True、use_pass_vector=True、cycles.use_denoising=True、Compositor 出现 3 个 File Output node、`format.color_management='OVERRIDE'` + `view_transform='Raw'`
    - **不**验证 `resolution_percentage` 改变(Apply Settings **不再**做这事;那是 T14 handler 在 render_init 时做的 save→override 流程)

  **Must NOT do**:
  - 不要给 user 暴露 `scale` slider(per 用户决议,scale 由 quality 推导)
  - **不要在 panel 显示**:输出 W/H(Blender Render Properties 的 `Resolution X/Y` 已有)、Halton phase 数、jitter X/Y per-frame 值、计算出来的 render_resolution、`resolution_percentage` 中间值(per 用户决议:UI 只暴露不可推导的、可交互的设置项)
  - 不要在 plugin panel 增加 `output_width` / `output_height` IntProperty(冗余于 `scene.render.resolution_x/y`)
  - 不要在面板里读 `dlssconfig.json`(那是 GUI 端的事)
  - 不要硬编码 quality 顺序为 alphabetical(必须 per DLSS doc 顺序:DLAA → UltraPerformance)
  - 不要使用 `EnumProperty` items 的 default value 是数字(用 enum string identifier)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Blender UI 设计,涉及 layout / row / toggle group 视觉布局选择
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: 15(dlssconfig writer 需读 properties)
  - **Blocked By**: 5(常量表), 7(骨架)

  **References**:
  - Blender Python API `bpy.props.EnumProperty` + `expand=True` for toggle row
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.7.1.1 line 980-984(scaling ratio table 是真实源,不要瞎填)
  - `src/core/types.h`(`DLSS_RATIO` 表必须一致)
  - **WHY**:面板是用户唯一 entry point,quality 选择直接影响 render_resolution + jitter_phases,**任何不一致都会导致管线失效**

  **Acceptance Criteria**:
  - [ ] panel.py 在 RENDER_PT_* 注册成功
  - [ ] panel header 含 enable checkbox(可绑 `props.enabled`,默认 False)
  - [ ] toggle group 横排显示 6 个 quality 按钮(正确顺序)
  - [ ] Apply Settings 按钮触发后:Compositor + Pass + Denoise + OCIO 全配置到位(**不**改 `resolution_percentage`,那是 T14 handler 职责)
  - [ ] `quality_mapping.py` 表与 `src/core/types.h` 字段值精确一致

  **QA Scenarios**:
  ```
  Scenario: Apply Settings 配置 pass + compositor + denoise(happy)
    Tool: Bash + bpy mock pytest
    Steps:
      1. mock scene + props.quality_mode = "performance"
      2. 触发 DLSS_OT_ApplySettings
      3. assert view_layer.use_pass_z == True
      4. assert view_layer.use_pass_vector == True
      5. assert scene.cycles.use_denoising == True
      6. assert Compositor 含 3 个 File Output node(Combined / Depth / Vector)
      7. assert File Output node format.color_management == 'OVERRIDE'
      8. assert File Output node view_settings.view_transform == 'Raw'
      9. **`scene.render.resolution_percentage` 不应被改变**(Apply Settings 不管这个;render_init handler 才管)
    Expected Result: 9 个断言全过
    Evidence: .omo/evidence/task-11-apply-settings-passes.txt

  Scenario: Quality 表与 DLSS_RATIO 公式一致性(failure prevention)
    Tool: pytest
    Steps:
      1. for each quality in [DLAA, UltraQuality, Quality, Balanced, Performance, UltraPerformance]:
         a. expected_pct = round(100 / DLSS_RATIO[quality])
         b. assert expected_pct in {100, 100, 67, 58, 50, 33}(对应 6 quality)
      2. 这些值在 T14 handler save→override 时被使用,确保 quality_mapping.py 公式正确
    Evidence: .omo/evidence/task-11-pct-formula.txt

  Scenario: render_resolution 整数算术(integer math)
    Tool: pytest
    Steps:
      1. output = 3840×2160, quality = Balanced (ratio 1.724),expected pct = 58
      2. 实际渲染像素 = floor(3840 * 58 / 100) = 2227
      3. dlssconfig.render_resolution.width 必须 = 2227(不是 3840/1.724 = 2227.38...)
    Evidence: .omo/evidence/task-11-int-math.txt

  Scenario: Quality table cross-check with C++ side(failure prevention)
    Tool: pytest + 读 src/core/types.h
    Steps:
      1. parse src/core/types.h 提取 DLSS_RATIO 字段
      2. compare with blender/dlss_compositor/quality_mapping.py DLSS_RATIO
      3. 全部相等(浮点 epsilon 1e-5)
    Expected Result: 0 mismatch
    Evidence: .omo/evidence/task-11-cross-check.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): quality panel (header toggle + quality + dir + apply for pass/compositor/denoise)`
  - Files: `blender/dlss_compositor/panel.py`、`operators.py`、`quality_mapping.py`、`blender/tests/test_quality_apply.py`

- [x] 12. Halton(2,3) jitter sequence,base=8,quality-mode 对应 phase 数

  **What to do**:
  - 写 `blender/dlss_compositor/jitter.py`:
    - `halton(index, base)` 函数(经典 van der Corput 实现)
    - `generate_jitter_sequence(quality_mode, frame_count)` 返回 List[(jitter_x, jitter_y)]:
      - phase_count = `JITTER_PHASES[quality_mode]`(per `quality_mapping.py`)
      - 对 frame N(从 0 计数):`phase = N % phase_count`;`jitter_x = halton(phase + 1, 2) - 0.5`;`jitter_y = halton(phase + 1, 3) - 0.5`
      - **必须 clamp** 到 `[-0.499, +0.499]` 防止 float 边缘越界(per Metis 决议)
    - `apply_jitter_to_frame(scene, frame_num, jitter_x, jitter_y)`:用 `scene.camera.delta_location` 或修改 `view_matrix`(选其一,前者更简单)在帧渲染前施加 jitter
  - 写 pytest:
    - 验证 `JITTER_PHASES` 表与 SR doc §3.7.1.1 line 980-984 完全匹配
    - 验证生成序列在 phase_count 帧后开始重复
    - 验证所有 jitter 值在 `[-0.499, +0.499]` 内
    - 验证序列分布:用 64 帧 Performance(phase 32)的序列,统计 jitter 散布在 [-0.5, +0.5] × [-0.5, +0.5] 矩形内 4 象限分布均匀(每象限 ≥ 25%)

  **Must NOT do**:
  - **不要用 base=1**(违反 §3.7.1.1)
  - 不要用 random / pseudo-random(必须 deterministic Halton)
  - 不要使用 base 与 phase_count 互质性不好的对(2 + 3 是 NVIDIA 推荐组合)
  - 不要把 jitter 烘进 motion vector(MV 必须是 pre-jitter 的)
  - 不要在 jitter.py 内加渲染逻辑(纯计算 + 一个 apply 函数)

  **Recommended Agent Profile**:
  - **Category**: `ultrabrain`
    - Reason: 数学精度敏感(Halton 序列、float clamp 边缘、相位匹配),错一点 SR 就退化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: 14(handler 触发 jitter)、15(写 jitter_x/y 到 dlssconfig)
  - **Blocked By**: 7

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.7.1(Halton 推荐 + line 967-984 phase count 表)
  - Halton sequence wiki: https://en.wikipedia.org/wiki/Halton_sequence
  - `src/core/types.h` `JITTER_PHASES` 表(必须一致)
  - **WHY**:Jitter 是 SR 的关键输入,错位 0.001 可以导致 SR 退化为 bilinear

  **Acceptance Criteria**:
  - [ ] `halton(index=1, base=2)` == `0.5` exactly
  - [ ] `halton(index=2, base=3)` ≈ `0.6666...`
  - [ ] 生成 32 帧 Performance 序列后,jitter 值范围 ⊂ [-0.499, +0.499]
  - [ ] 第 33 帧的 jitter == 第 1 帧(phase 32 wrap)
  - [ ] `JITTER_PHASES` table 6 项与 SR doc table 完全一致(per quality assertion)

  **QA Scenarios**:
  ```
  Scenario: Halton 经典值验证(happy)
    Tool: pytest
    Steps:
      1. halton(1, 2) == 0.5 (exact)
      2. halton(2, 2) == 0.25
      3. halton(3, 2) == 0.75
      4. halton(1, 3) ≈ 0.3333
      5. halton(2, 3) ≈ 0.6666
    Evidence: .omo/evidence/task-12-halton-classic.txt

  Scenario: 序列分布均匀(happy)
    Tool: pytest
    Steps:
      1. 生成 Performance 模式 32 帧 jitter 序列
      2. 统计 4 象限分布(jitter > 0 或 < 0,X/Y 各 2 象限组合 4 类)
      3. 每象限 frame 数 ≥ 6(理论 8,允许 25% 偏差)
    Evidence: .omo/evidence/task-12-distribution.txt

  Scenario: Phase 数与 quality 严格对应(failure prevention)
    Tool: pytest
    Steps:
      1. assert JITTER_PHASES[DLAA] == 8
      2. assert JITTER_PHASES[Quality] == 18
      3. assert JITTER_PHASES[Balanced] == 24
      4. assert JITTER_PHASES[Performance] == 32
      5. assert JITTER_PHASES[UltraPerformance] == 72
      6. assert JITTER_PHASES[UltraQuality] == 8(同 DLAA,由 §3.7.1 推断)
    Expected Result: 6/6 通过
    Evidence: .omo/evidence/task-12-phase-counts.txt

  Scenario: 越界 clamp(edge case)
    Tool: pytest
    Steps:
      1. mock halton 返回 1.0(理论不会但模拟边缘)
      2. apply: jitter_x = 1.0 - 0.5 = 0.5
      3. clamp 应将 0.5 → 0.499
    Evidence: .omo/evidence/task-12-clamp.txt

  Scenario: 不能用 base=1(failure prevention)
    Tool: grep + ast_grep
    Steps:
      1. grep -nE "base\s*=\s*1\b" blender/dlss_compositor/jitter.py → 0 匹配
      2. ast_grep --lang python 'jitter_phases = $$$ * scale * scale' → 0 匹配(不该有这种 base=1 计算)
    Evidence: .omo/evidence/task-12-no-base-1.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): Halton(2,3) jitter base=8 with per-quality phase counts`
  - Files: `blender/dlss_compositor/jitter.py`、`blender/tests/test_jitter.py`

  > **重要**:T11 panel "Apply Settings" 按钮 内部直接调用 T13 的 `DLSS_OT_ConfigureAllPasses` operator(两者**统一为同一动作**;UI 上以 "Apply Settings" 标签呈现给用户,内部实现复用 T13 operator)。T13 的 operator 单独存在以便:(a) 解耦 panel UI 与 operator 实现;(b) 提供给 Blender Python API 直接调用。**用户视角:只点 "Apply Settings",不暴露独立的 "Configure All Passes" 按钮**(per T41 docs "Configure All Passes 已合并入 Apply Settings")。
- [x] 13. Configure All Passes operator + Vector pass 通道 sanity 实证

  **What to do**:
  - 写 `blender/dlss_compositor/operators.py` 加 `DLSS_OT_ConfigureAllPasses` operator:
    - 启用 view layer pass:`view_layer.use_pass_z = True`、`view_layer.use_pass_vector = True`,Combined 总是开
    - 强制 cycles denoise:`scene.cycles.use_denoising = True`(per 用户决议:Cycles denoise 必开)
    - 在 compositor 中创建/查找 `File Output` 节点,接 RenderLayers 的 `Combined` / `Depth` / `Vector` 输出:
      - `Combined` → `<output_dir>/<frame>.exr`(R16G16B16A16_HALF / FLOAT)
      - `Depth` → `<output_dir>/<frame>.depth.exr`(R32_FLOAT)
      - `Vector` → `<output_dir>/<frame>.vector.exr`(RGBA32_FLOAT,4 通道全部)
    - 强制 File Output node 的 `format.color_management = 'OVERRIDE'` + `view_settings.view_transform = 'Raw'`(per Metis 决议:防 "Save as Render" 污染 EXR scene-linear)
    - filename pattern 用 `####.exr`(4 位 zero-padded)
  - 在 panel 加 `Configure All Passes` 按钮触发该 operator(per T11 决议:T11 的 "Apply Settings" 按钮内部调用此 operator;**用户视角只看到 Apply Settings**,T13 operator 仍单独存在以解耦 UI / API 调用)
  - **Vector pass 通道 sanity 实证测试**(per Metis §2e + 用户提供的 stackexchange 信息):
    - 写 `blender/tests/test_vector_pass_sanity.py`(用 Blender headless 跑,**不是 mock**)
    - 构造极简场景:1 个 plane 在 origin,1 个 camera 朝向 +Z
    - 帧 1 plane 在 (0, 0, 0);帧 2 plane 在 (5, -3, 0)(Blender 世界坐标 +X 是右,但屏幕坐标视情况)
    - 渲染 2 帧,读 frame_0002 vector pass:**断言中心像素的 Vector.X 与 Vector.Y 与"DLSS 期望的 (Blender.X, -Blender.Y) curr→prev MV(标准 +X=右, +Y=下)"推导一致**
    - 推导逻辑:plane 在屏幕上向"右"移动(假设 camera 朝 +Z),Blender Vector pass 因 X 正=左 → Vector.X = 负值;经映射 `DLSS.X = Blender.X` 后 DLSS.X 也是负值;curr→prev 期望 DLSS.X 为负(prev 在左) → ✅ 一致
    - **如果实证失败**(MV 方向与推导相反):pytest 失败,**T23 必须用实证结果而非推导写映射**,文档化最终符号

  **Must NOT do**:
  - 不要假设 Blender 用户已开 denoise(强制设)
  - 不要把 vector pass 4 通道压成 2 通道(EXR 写 4 通道,T23 在 GPU 端做 2 通道映射)
  - 不要让 File Output 走默认 Filmic(必须 Raw + scene-linear EXR)
  - 不要给 sanity test 用 mock(必须真 Blender headless)
  - 不要在 sanity test 里调 DLSS(那是 T23/T24 的集成)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 跨 Blender API + EXR 解析 + 符号约定推理 + 实证验证;一旦写错全管线 SR 退化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: 23(MV 转换 shader 必须用 sanity 验证后的映射)
  - **Blocked By**: 7

  **References**:
  - 用户提供的 Blender StackExchange 答案(Vector.X 正=左, Y 正=下, prev→curr in pixels)
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.6(MV pixel space, screen-space, curr→prev, X 正=右)
  - Blender Cycles Vector pass docs
  - **WHY**:v1 audit 标 "MV 符号转换实验结论,文档矛盾",v2 必须实证一次,把结论钉死在测试里防回归

  **Acceptance Criteria**:
  - [ ] `DLSS_OT_ConfigureAllPasses` operator 注册成功
  - [ ] 调用后:`view_layer.use_pass_z == True`、`use_pass_vector == True`、`use_denoising == True`
  - [ ] Compositor 含 3 个 File Output 节点(Combined / Depth / Vector)
  - [ ] File Output `format.color_management == 'OVERRIDE'`、`view_transform == 'Raw'`
  - [ ] 实证 sanity test 通过:plane 右移 → Vector.X 与 (Blender.X, -Blender.Y) 推导的 DLSS curr→prev 期望一致
  - [ ] sanity test 输出 markdown 报告 `blender/tests/MV_SANITY_RESULT.md`(实证结论 + plane 像素值 + 符号映射 final 结论)

  **QA Scenarios**:
  ```
  Scenario: Configure All Passes 一键配置(happy)
    Tool: pytest + bpy mock
    Steps:
      1. 调 DLSS_OT_ConfigureAllPasses
      2. 断言 6 项设置全到位
      3. 断言 3 个 File Output node 出现
    Evidence: .omo/evidence/task-13-configure-passes.txt

  Scenario: Vector 通道实证(integration, 真 Blender)
    Tool: Bash + blender --background
    Steps:
      1. blender --background --python blender/tests/test_vector_pass_sanity.py
      2. 解析 frame_0002 vector EXR,读中心像素(plane 移动后的中心)
      3. 断言 Vector.X 符号与 plane 屏幕方向一致(用户 stackexchange 信息)
      4. 断言 (Blender.X, -Blender.Y) 推导后 DLSS curr→prev 期望符号正确
    Expected Result: 推导映射符号验证通过,生成 MV_SANITY_RESULT.md
    Evidence: .omo/evidence/task-13-vector-sanity.log + MV_SANITY_RESULT.md

  Scenario: Color management OVERRIDE 强制(failure prevention)
    Tool: pytest
    Steps:
      1. 用户故意 enable "Save As Render"
      2. 触发 ConfigureAllPasses
      3. 断言 file_output.format.color_management == 'OVERRIDE'(覆盖了用户设置)
    Evidence: .omo/evidence/task-13-color-management.txt

  Scenario: 实证失败时 abort(failure path)
    Tool: pytest
    Steps:
      1. 模拟 Vector.X 符号与推导相反(篡改 EXR 数据)
      2. sanity test 应失败
      3. T23 必须有显式 hard dep 文件(`MV_SANITY_RESULT.md`),Wave 3 不能在 sanity 失败时进
    Evidence: .omo/evidence/task-13-sanity-fails-blocks-T23.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): one-click pass configuration + Vector channel empirical sanity`
  - Files: `blender/dlss_compositor/operators.py`(updated)、`panel.py`(updated)、`blender/tests/test_vector_pass_sanity.py`、`blender/tests/MV_SANITY_RESULT.md`(测试 produced)

- [x] 14. render handler 静默 + atomic merge

  **What to do**:
  - 写 `blender/dlss_compositor/handlers.py`:
    - **核心模式:save → override → restore(per 用户决议:"渲染前记住用户设置的百分比,改成我们设置的百分比,渲染完或中断还原")**
    - `dlss_render_init(scene)`(`bpy.app.handlers.render_init`,在整个 render 队列开始前触发一次):
      - 若 `props.enabled == False` → 全 no-op,return
      - 把 `scene.render.resolution_percentage` 持久化到 `scene["_dlss_saved_pct"]`(custom property,**survives Blender 崩溃**)
      - 计算 `dlss_pct = round(100 / DLSS_RATIO[props.quality_mode])`
      - `scene.render.resolution_percentage = dlss_pct`
      - LOG_INFO `DLSS engaged: original {orig}% → {dlss}% (Quality={mode})`
    - `dlss_render_pre(scene)`(`bpy.app.handlers.render_pre`,每帧渲染前):
      - 若 `props.enabled == False` 或 `scene["_dlss_saved_pct"]` 不存在 → no-op
      - 计算当前 frame_num 的 jitter(调 T12 的 `generate_jitter_sequence`)
      - apply jitter to camera(调 `apply_jitter_to_frame`)
      - 收集 per-frame 数据:`matrix_world`(4×4)、`projection`(4×4 from `camera.calc_matrix_camera`)、`fov`、`aspect_ratio`、`near_clip`、`far_clip`、`jitter_x`、`jitter_y`
      - 写到 `<output_dir>/frames/<frame_num:04d>.cam.json`(单帧文件,**避免对总 dlssconfig.json 的并发写**)
      - 写时用 atomic rename:先写 `.cam.json.tmp` → `os.replace()`
    - `dlss_render_complete(scene)` 与 `dlss_render_cancel(scene)` 共享 finalize 逻辑 — **唯一的 dlssconfig.json 写入点 + percentage 还原点**:
      - 若 `scene["_dlss_saved_pct"]` 不存在 → no-op(insurance:enabled=False 时不该到这)
      - 扫描 `<output_dir>/frames/*.cam.json`,按 frame_num 排序,合并到 `<output_dir>/dlssconfig.json`(顶层 fields + `frames` dict);用 atomic write
      - **Restore**:`scene.render.resolution_percentage = scene["_dlss_saved_pct"]`,然后 `del scene["_dlss_saved_pct"]`
      - 删除 per-frame `.cam.json` 文件(可选,留作 audit)
      - LOG_INFO `DLSS finalized: dlssconfig.json written ({n} frames), restored % to {orig}`
    - **崩溃恢复 `dlss_load_post(scene)`**(`bpy.app.handlers.load_post`,Blender 启动 / 加载 .blend 后触发):
      - 若 `scene["_dlss_saved_pct"]` 仍存在 → 说明上次 render 异常退出未 restore
      - LOG_WARN `DLSS detected stale saved_pct ({saved}%, current {current}%); restoring`
      - `scene.render.resolution_percentage = scene["_dlss_saved_pct"]`
      - `del scene["_dlss_saved_pct"]`
    - `register_handlers()` / `unregister_handlers()`:用 `bpy.app.handlers.render_init.append`、`render_pre.append`、`render_complete.append`、`render_cancel.append`、`load_post.append`
  - 在 `__init__.py` 的 register/unregister 调 `register_handlers` / `unregister_handlers`
  - pytest mock 验证:
    - enabled=True 5 帧渲染 → 5 个 cam.json → render_complete 触发后 dlssconfig.json 出现 + per-frame 数据齐全 + percentage 还原回原值
    - enabled=False 5 帧渲染 → 0 个 cam.json + dlssconfig.json 不存在 + percentage 不变
    - 模拟中断(render_cancel) → dlssconfig.json 含部分结果 + percentage 还原
    - 模拟崩溃(只触发 render_init,不触发 finalize)→ scene["_dlss_saved_pct"] 残留 → 模拟 load_post → 自动恢复 + 清除 custom prop

  **Must NOT do**:
  - **不要直接每帧写 dlssconfig.json**(快速渲染时多 frame 并发写会竞态;且单 task 是高频写入磁盘浪费 IO,per 用户决议:**仅在 render_complete 或 render_cancel 时写 dlssconfig.json**,渲染期间只写 per-frame `frames/<N>.cam.json`)
  - **不要在 handler 抛异常**(Blender 会卡住),用 try/except + LOG_WARN
  - 不要假设 output_dir 存在(自动 mkdir -p)
  - **不要在合成时使用 `json.dump` 不带 atomic rename**(中途崩溃会留半文件)
  - 不要往 dlssconfig 写 `cut_after`(per Metis Q3 决议)
  - 不要让 render_pre 修改 jitter 之外的 scene state(单一职责)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: 文件 I/O + bpy handler 注册,模板化但小心 atomic 模式
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: 16(handler 测试)
  - **Blocked By**: 7, 12, **15**(T14 handler 调用 T15 dlssconfig_writer 函数,T15 必须先实现 — 与 T15 自身 "Blocked By 注" 一致)

  **References**:
  - Blender `bpy.app.handlers` docs
  - Python `os.replace()` for atomic rename
  - Metis 决议:per-frame 文件 + 渲染结束 atomic merge
  - **WHY**:v1 用单个 camera.json 让用户手动导出,v2 改静默 + per-frame 防竞态;atomic rename 是关键防崩溃工具

  **Acceptance Criteria**:
  - [ ] 5 个 handlers attach 成功:`render_init`、`render_pre`、`render_complete`、`render_cancel`、`load_post`
  - [ ] **enabled=True save/override 流程**:render_init 触发后 → `scene["_dlss_saved_pct"]` 含原始值 + `scene.render.resolution_percentage` 已变成 quality 推导值
  - [ ] **enabled=True restore 流程**:render_complete / render_cancel 触发后 → `scene.render.resolution_percentage` 还原为 saved_pct + `_dlss_saved_pct` custom prop 删除
  - [ ] **enabled=False no-op**:任何 handler 触发都 no-op,用户 percentage 完全不变
  - [ ] **崩溃恢复**:模拟 init 后无 finalize → `_dlss_saved_pct` 残留 → load_post 触发 → 自动恢复 + 清除
  - [ ] 5 帧渲染期间:`frames/0001.cam.json` ~ `frames/0005.cam.json` 存在 + 内容齐全(matrix_world / jitter / fov 等)
  - [ ] render_complete 后 `dlssconfig.json` 出现,顶层字段 + 5 个 frame entries
  - [ ] 中途 render_cancel 触发 → dlssconfig.json 含已渲染部分 + percentage 还原
  - [ ] 任何 handler 都不抛异常

  **QA Scenarios**:
  ```
  Scenario: enabled=True full lifecycle(happy critical)
    Tool: pytest + bpy mock
    Steps:
      1. mock scene + props.enabled=True + props.quality_mode='performance' + scene.render.resolution_percentage=80
      2. 触发 render_init → assert scene["_dlss_saved_pct"]==80, scene.render.resolution_percentage==50
      3. for frame 1..5: 触发 render_pre → cam.json 写入
      4. 触发 render_complete → dlssconfig.json merged (5 frames),scene.render.resolution_percentage==80(还原),scene 无 _dlss_saved_pct prop
    Expected Result: 4 步全过
    Evidence: .omo/evidence/task-14-full-lifecycle.txt

  Scenario: enabled=False 完全 no-op(critical)
    Tool: pytest
    Steps:
      1. mock scene + props.enabled=False + scene.render.resolution_percentage=80
      2. 触发 render_init → assert scene 无 _dlss_saved_pct prop, percentage 不变
      3. for frame 1..3: render_pre → frames/ 目录不存在或为空
      4. render_complete → dlssconfig.json 不存在,percentage=80
    Expected Result: 4 步全过
    Evidence: .omo/evidence/task-14-disabled-noop.txt

  Scenario: render_cancel 部分结果 + restore(happy edge)
    Tool: pytest
    Steps:
      1. enabled=True,启动 render_init,frame 1-3 完成 render_pre,然后 render_cancel
      2. dlssconfig.json 含 3 frame entries
      3. percentage 还原,_dlss_saved_pct 已删
    Evidence: .omo/evidence/task-14-cancel-restore.txt

  Scenario: 崩溃恢复 via load_post(failure path)
    Tool: pytest
    Steps:
      1. 模拟 render_init 触发后 process killed(unit test 直接设 _dlss_saved_pct=80, percentage=50)
      2. 模拟 Blender 重启 → 触发 dlss_load_post(scene)
      3. assert percentage 自动恢复为 80,_dlss_saved_pct 已删
      4. LOG_WARN 含 "stale saved_pct" 字样
    Evidence: .omo/evidence/task-14-crash-recovery.txt

  Scenario: Atomic rename 中断不留半文件(failure prevention)
    Tool: pytest
    Steps:
      1. mock os.replace 第二次调用 raise IOError
      2. 触发 render_complete
      3. 验证目录无 .tmp 残留(或失败时保留 .tmp 但 dlssconfig.json 无被损坏的中间态)
    Evidence: .omo/evidence/task-14-atomic-resilience.txt

  Scenario: 不写 cut_after(failure prevention)
    Tool: grep
    Steps:
      1. grep -rE "cut_after" blender/dlss_compositor/handlers.py → 0 matches
      2. mock 5 帧渲染合成结果中无 cut_after 字段
    Evidence: .omo/evidence/task-14-no-cut-after.txt

  Scenario: 并发写不竞态(integration, real Blender if possible)
    Tool: blender --background script
    Steps:
      1. 真 Blender 渲染 30 帧动画
      2. 30 个 frames/*.cam.json 全部出现,无半截文件
      3. 合并后 dlssconfig.json 含 30 个 frame entries(无重复 / 缺失)
      4. percentage 还原回 user 设置值
    Evidence: .omo/evidence/task-14-integration-30frames.log
  ```

  **Commit**: YES
  - Message: `feat(blender): silent render handlers + atomic dlssconfig merge`
  - Files: `blender/dlss_compositor/handlers.py`、`blender/tests/test_handlers.py`

- [x] 15. dlssconfig.json writer + schema validation(Blender side)

  **What to do**:
  - 写 `blender/dlss_compositor/dlssconfig_writer.py`:
    - `build_top_level_config(scene, props)` → dict:
      - `version: 1`
      - `output_resolution: { width: scene.render.resolution_x, height: scene.render.resolution_y }`(直接从 Blender Render Properties 读,**插件 panel 不暴露 output W/H**)
      - `render_resolution: { width: scene.render.resolution_x * scene.render.resolution_percentage / 100, height: ... }`(整数算术,per Metis Q1)
      - `quality_mode: props.quality_mode.lower()`(snake_case 字符串)
      - `display_device: scene.display_settings.display_device`(per Blender 5.1 docs:https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html)
      - `view_transform: scene.view_settings.view_transform`(per Blender 5.1 docs:https://docs.blender.org/manual/en/latest/render/color_management/color_spaces.html)
      - **`is_hdr` 推导逻辑**(per 用户提供的 Blender 5.1 color management docs):
        ```python
        # is_hdr=True ONLY if display_device 表明 HDR-capable display
        # Blender 5.1 ships display devices: sRGB, Rec.2100-PQ, Rec.2100-HLG, ACEScg-cct, etc.
        # HDR display devices: Rec.2100-PQ, Rec.2100-HLG (and any future BT.2100 variants)
        HDR_DISPLAY_DEVICES = {'Rec.2100-PQ', 'Rec.2100-HLG'}
        is_hdr = display_device in HDR_DISPLAY_DEVICES
        ```
      - `fps: scene.render.fps`
      - `filename_pattern: r"^\d{4}\.exr$"`(与 T13 配置一致)
      - `jitter_phases: JITTER_PHASES[quality_mode]`(per T12)
    - `write_atomic(path, data)`:json.dump 到 .tmp 然后 os.replace
    - `validate_against_schema(data)`:加载 `src/core/dlssconfig.schema.json`,用 `jsonschema` Python 库 validate;失败抛 `ValueError` with JSON pointer
    - `merge_per_frame_jsons(output_dir)`:扫描 `frames/*.cam.json`,合并 `frames` dict
  - **关键**:Blender 端 schema 验证使用 `src/core/dlssconfig.schema.json` 作为 single source of truth(从 v2/main repo 内引用,不复制)
  - 写 pytest:验证 4 个 happy + 6 个 schema reject case(jitter 越界 silent clamp、缺字段、render>output、frames key 不匹配 pattern、is_hdr 与 display_device 不匹配、version != 1)+ 5 个 color management combos(sRGB→is_hdr=False,Rec.2100-PQ→is_hdr=True,等)
  - **Jitter clamp**:写入前 `jitter_x = max(-0.499, min(0.499, jitter_x))`(per Metis decision,防 schema 验证拒绝边缘 0.5 值)

  **Must NOT do**:
  - 不要让 `is_hdr` 由用户手动配(从 `display_device` 推导)
  - 不要把 schema 复制到 blender/(必须引用 `src/core/dlssconfig.schema.json` single source)
  - 不要在写入前不 clamp jitter
  - 不要在 `quality_mode` 字段写 enum 数字(必须 string)
  - 不要省略 schema validation 步骤(写入前必校验,失败拒绝)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: dict 构造 + atomic write + schema validation,标准 Python
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: **14**, 16(T14 handler 调用 T15 writer,T15 必须先实现;T16 测试)
  - **Blocked By**: 5(schema), 11(properties), 12(jitter)。**注:T14 不是依赖**(T14 反过来调用 T15 的 writer 函数,T15 必须先实现)

  **References**:
  - `src/core/dlssconfig.schema.json`(T5 single source of truth)
  - Python `jsonschema` lib
  - Blender `bpy.context.scene.display_settings` / `view_settings`
  - **Blender 5.1 color management docs**(per 用户提供):
    - https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html(display_device + view_transform 的语义)
    - https://docs.blender.org/manual/en/latest/render/color_management/color_spaces.html(color spaces 列表)
    - is_hdr 仅 BT.2100-based display device 才为 true
  - **WHY**:此 task 是 Blender → CLI 契约的实际写入点,必须严格遵守 schema 防止 GUI/CLI 解析错乱;is_hdr 推导直接影响 Stage 4 EOTF 选择(sRGB vs PQ),错则 FG 输入 display-ready 不正确

  **Acceptance Criteria**:
  - [ ] `build_top_level_config(scene, props)` 输出可被 `jsonschema` validate 通过
  - [ ] schema validation 在写入前调用;失败 → `LOG_WARN` + 抛 `ValueError`,文件不写
  - [ ] jitter 越界值 0.5001 自动 clamp 到 0.499(no error)
  - [ ] `is_hdr` 推导:display_device='sRGB' → `is_hdr=False`;'Rec.2100-PQ' → `is_hdr=True`
  - [ ] 6 个 reject case 测试全部精确触发对应错误

  **QA Scenarios**:
  ```
  Scenario: 写 + 读 round-trip(happy)
    Tool: pytest
    Steps:
      1. 构造 props (4K Performance) + 5 frames 数据
      2. 调 build_top_level_config + merge_per_frame_jsons
      3. write_atomic 到 dlssconfig.json
      4. 读回 + 用 jsonschema validate → 通过
      5. 字段值与输入一致
    Evidence: .omo/evidence/task-15-roundtrip.txt

  Scenario: Schema reject paths(failure)
    Tool: pytest
    Steps:
      1. version != 1 → reject
      2. 缺 fps → reject
      3. render_resolution.width > output_resolution.width → reject
      4. jitter_x = 0.7 in frame data → **silent clamp 到 0.499 + LOG_WARN**(per Metis,与 T6 一致)
      5. frames["abc"] (不匹配 \\d{4}) → reject
      6. is_hdr=True 但 display_device='sRGB' → reject(inconsistency 检查)
    Expected Result: 5 个 reject + 1 个 clamp + warn
    Evidence: .omo/evidence/task-15-schema-rejects.txt

  Scenario: Schema 是引用,非复制(failure prevention)
    Tool: Bash
    Steps:
      1. md5sum src/core/dlssconfig.schema.json | tee a
      2. find blender/ -name "*.json" -path "*schema*" | xargs md5sum | tee b
      3. b 应是空(blender 端不复制 schema)
    Evidence: .omo/evidence/task-15-schema-not-duplicated.txt

  Scenario: Jitter clamp 静默(edge)
    Tool: pytest
    Steps:
      1. 输入 jitter_x = 0.5001
      2. writer 应 clamp 到 0.499 + LOG_WARN
      3. 输出文件含 0.499,验证通过
    Evidence: .omo/evidence/task-15-jitter-clamp.txt
  ```

  **Commit**: YES
  - Message: `feat(blender): dlssconfig writer with schema validation + jitter clamping`
  - Files: `blender/dlss_compositor/dlssconfig_writer.py`、`blender/tests/test_dlssconfig_writer.py`

- [x] 16. pytest + bpy mock infrastructure 完整搭建

  **What to do**:
  - 写 `blender/tests/conftest.py`:
    - 配置 **`bpy-stubs`**(用户提供路径:`C:\Python313\Lib\site-packages\bpy-stubs\__init__.pyi`)为 mock 库(import 时不需要真 Blender);本地 dev 用 `python -c "import sys; sys.path.insert(0, r'C:\\Python313\\Lib\\site-packages\\bpy-stubs')"` 或修改 conftest sys.path
    - fallback 链:bpy-stubs → fake-bpy-module(若 stubs 缺某些 API)
    - 提供 fixtures:`fake_scene`(可配置 resolution / fps / display_device)、`fake_render_props`(quality_mode 等)、`tmp_output_dir`、`mock_camera_at(loc, rot)`、`mock_color_management(display_device, view_transform)`(用于 is_hdr 测试)
  - 写 `blender/tests/test_integration.py` 大整合:
    - 安装 + register
    - 配置 4K Balanced
    - Configure All Passes
    - 模拟渲染 30 帧(handler 触发)
    - 渲染结束触发 merge
    - 解析 dlssconfig.json:30 个 frame entries + jitter 在范围 + render_resolution 与 quality 一致(整数算术)
    - 与 `src/core/dlssconfig.schema.json` 双向 validate
  - 写 `blender/tests/cross_check_constants.py`:
    - 解析 `src/core/types.h` 提取 `DLSS_RATIO` 等表
    - 比较 `blender/dlss_compositor/quality_mapping.py` 表
    - 任何不一致 → fail with diff
  - 写 `blender/tests/README.md`:本地跑测试的命令(`python -m pytest blender/tests/`)
  - 在 CI(T10 写的 workflow)中**暂跳过 blender pytest**(不装 Blender)— 但本地必须能跑通

  **Must NOT do**:
  - 不要 mock 太深以至于测试不跑实际 register 路径(register 是关键 entry point)
  - 不要在 conftest 引入 GPU 依赖(全部 CPU mock)
  - 不要要求 CI 跑 Blender 集成测试(成本太高,本地跑)
  - 不要让 cross_check_constants 用 fragile string 解析(用 regex 提取常量值,允许 c++ 注释/空格变化)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: 测试基建,模板化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2
  - **Blocks**: 无(测试)
  - **Blocked By**: 7, 11, 12, 13, 14, 15

  **References**:
  - `bpy-stubs`(用户已装于 `C:\Python313\Lib\site-packages\bpy-stubs\`,优先用此)
  - `fake-bpy-module`(fallback,pip install)
  - pytest fixtures docs
  - 用户决议 Q4(test fixtures cherry-pick)
  - **WHY**:Blender 插件复杂度高,需要本地快速反馈循环;CI 不跑 Blender 但本地必须

  **Acceptance Criteria**:
  - [ ] `python -m pytest blender/tests/ -v` 本地全过(假设 bpy-stubs 已装于 `C:\Python313\Lib\site-packages\bpy-stubs\`)
  - [ ] `cross_check_constants.py` 验证 quality_mapping.py 与 src/core/types.h 一致
  - [ ] integration test 模拟 30 帧 → dlssconfig.json schema 验证通过
  - [ ] CI workflow 不依赖 Blender(blender pytest 不在 build-test.yml 中)

  **QA Scenarios**:
  ```
  Scenario: 本地完整测试套件(happy)
    Tool: Bash + pytest
    Steps:
      1. 验证 C:\Python313\Lib\site-packages\bpy-stubs\__init__.pyi 存在
      2. python -m pytest blender/tests/ -v
      3. 所有 test (test_register / test_quality_apply / test_jitter / test_handlers / test_dlssconfig_writer / test_integration / cross_check_constants) 全过
    Expected Result: 0 failures
    Evidence: .omo/evidence/task-16-full-pytest.txt

  Scenario: cross_check_constants 抓不一致(failure prevention)
    Tool: pytest
    Steps:
      1. 故意改 quality_mapping.py 的 Performance ratio 为 1.9(应是 2.0)
      2. 跑 cross_check_constants → 失败,打印 diff
    Evidence: .omo/evidence/task-16-cross-check-fails-on-mismatch.txt

  Scenario: 30 帧 integration 端到端(happy integration)
    Tool: pytest
    Steps:
      1. mock 4K Balanced + 30 frames
      2. 跑 test_integration::test_full_render
      3. 验证最终 dlssconfig.json:render_resolution = 2227×1253(round(3840*58/100)),frames count 30,jitter 全在 [-0.499, +0.499]
    Evidence: .omo/evidence/task-16-30frame-integration.txt
  ```

  **Commit**: YES
  - Message: `test(blender): pytest + bpy-mock test suite + cross-language constant check`
  - Files: `blender/tests/conftest.py`、`test_integration.py`、`cross_check_constants.py`、`README.md`

- [x] 17. Vulkan context(volk + VMA, compute-only,无 swapchain)

  **What to do**:
  - 写 `src/gpu/vulkan_context.h` + `vulkan_context.cpp`:
    - `class VulkanContext`:
      - ctor: 选 NVIDIA RTX physical device(检查 device extensions:`VK_KHR_external_memory_win32`, `VK_KHR_synchronization2`, `VK_NV_memory_decompression` 可选)
      - 创建 logical device + 1 个 graphics+compute queue + 1 个 transfer queue
      - 初始化 VMA allocator
      - **不创建 swapchain / surface / GLFW**(headless,compute-only)
      - **暴露 NGX 所需 extensions**:在 device create info 加入 `NVSDK_NGX_VULKAN_GetDeviceExtensions` 返回的扩展 + `NVSDK_NGX_VULKAN_GetInstanceExtensions` 返回的扩展(per DLSS doc Vulkan setup §3.4)
    - 提供 helper:
      - `Cmd beginOneTimeCommands()` / `endOneTimeCommands(Cmd)`(transfer queue)
      - `Cmd beginComputeCommands()` / `endComputeCommands(Cmd)`(compute queue,NGX 用此)
      - `void waitIdle()`
    - Validation layer 在 Debug 自动开,Release 关
    - debug callback 用 spdlog `LOG_*`
  - 写 Catch2 `tests/test_vulkan_context.cpp`:
    - 创建 context → 验证 NVIDIA device 选中
    - 检查 NGX-required extensions 都启用
    - 创建 1 个 image + upload data + readback → 数据一致
    - destruct → 无 leak(VMA stats == 0)

  **Must NOT do**:
  - **不要创建 swapchain / surface / window**(v1 已删除,v2 不复活)
  - 不要使用 vulkan-hpp(用纯 C API + volk)
  - 不要硬编码 device index(选第一个 NVIDIA RTX 卡)
  - 不要假设有 graphics queue(NGX 需要 compute queue,fallback graphics+compute)
  - 不要 ignore NGX extension query(`NVSDK_NGX_VULKAN_GetInstanceExtensions/DeviceExtensions` 必须调,否则 NGX init 会失败)

  **Recommended Agent Profile**:
  - **Category**: `ultrabrain`
    - Reason: Vulkan device init + NGX extension list 整合 + 无 swapchain pattern,极易写错
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 18-29(所有 GPU task 依赖 Vulkan context)
  - **Blocked By**: 3

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.4(Vulkan resource creation)
  - `DLSS/include/nvsdk_ngx_vk.h`(`NVSDK_NGX_VULKAN_GetInstanceExtensions/DeviceExtensions` 函数签名)
  - VMA docs: https://gpuopen.com/vulkan-memory-allocator/
  - volk docs: https://github.com/zeux/volk
  - **WHY**:NGX 要求特定 Vulkan extensions,**必须在 createDevice 之前查询并合并**;v1 已正确实现此模式可参考但 v2 重写

  **Acceptance Criteria**:
  - [ ] `VulkanContext` 创建成功,验证 layer 在 Debug 启用
  - [ ] 选中 NVIDIA RTX 设备(physical_device.properties.vendorID == 0x10DE)
  - [ ] 启用了 NGX query 返回的所有 extensions
  - [ ] image upload+readback 测试通过(像素 hash 一致)
  - [ ] destruct 后 VMA `getStatistics().total.statistics.allocationCount == 0`

  **QA Scenarios**:
  ```
  Scenario: Vulkan context 创建 + NGX extensions(happy)
    Tool: Catch2
    Preconditions: 测试机有 RTX GPU,Vulkan SDK 1.3+ 装好
    Steps:
      1. 创建 VulkanContext()
      2. 验证 selected_physical_device.properties.vendorID == 0x10DE
      3. NVSDK_NGX_VULKAN_GetDeviceExtensions 返回 N 个,VulkanContext 启用了至少这 N 个
      4. 创建 32x32 R8G8B8A8_UNORM image, upload pattern, copy back, hash 一致
    Evidence: .omo/evidence/task-17-vulkan-init.txt

  Scenario: 无 RTX GPU 时 graceful 错误(failure)
    Tool: Catch2 (mock 阻断 device enum)
    Steps:
      1. mock vkEnumeratePhysicalDevices 返回 0 设备
      2. 创建 VulkanContext → 应抛 std::runtime_error("No NVIDIA RTX GPU found")
    Evidence: .omo/evidence/task-17-no-rtx-graceful.txt

  Scenario: 无 swapchain / surface(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'vkCreateSwapchainKHR($$$)' src/ → 0 匹配
      2. ast_grep --lang cpp 'vkCreateWin32SurfaceKHR($$$)' src/ → 0 匹配
      3. grep -r "GLFW" src/ → 0 匹配
    Evidence: .omo/evidence/task-17-no-swapchain.txt

  Scenario: VMA leak 检测(failure prevention)
    Tool: Catch2
    Steps:
      1. 创建 + destruct context,中间分配 + free 100 个 image
      2. 验证 vma stats final allocationCount == 0
    Evidence: .omo/evidence/task-17-no-leak.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): Vulkan compute-only context with NGX-required extensions`
  - Files: `src/gpu/vulkan_context.h`、`src/gpu/vulkan_context.cpp`、`tests/test_vulkan_context.cpp`、`CMakeLists.txt`(链 volk/vma)

- [x] 18. NGX 初始化封装(SR + FG, dev/release DLL switch)

  **What to do**:
  - 写 `src/dlss/ngx_wrapper.h` + `ngx_wrapper.cpp`:
    - `class NgxContext`:
      - ctor: 调 `NVSDK_NGX_VULKAN_Init_with_ProjectID(...)` // §3.5 推荐用 with_ProjectID
        - 参数:project_id="dlss-compositor-v2"、engine_type=NVSDK_NGX_ENGINE_TYPE_CUSTOM、engine_version=appver::FULL_STRING
        - 注释引用 `// DLSS SR §3.5`
      - 调 `NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)`,缓存 params
      - 检查 `NVSDK_NGX_Parameter_SuperSampling_Available` → bool `m_srAvailable`
      - 检查 `NVSDK_NGX_Parameter_FrameGeneration_Available` → bool `m_fgAvailable`
      - 检查 `NVSDK_NGX_Parameter_FrameGeneration_MultiFrameCountMax` → `m_fgMaxMultiplier`(支持 2x/3x/4x 检测,per FG §1 line 429)
      - 输出 NGX banner 到 LOG_INFO:`[NGX] DLSS SR: AVAILABLE | FG: AVAILABLE max=4x | DLL nvngx_dlss.dll v3.10.5 | nvngx_dlssg.dll v3.10.5`
    - dtor: `NVSDK_NGX_VULKAN_Shutdown1()`(per §5.4)
    - getter:`isSrAvailable()` / `isFgAvailable()` / `getFgMaxMultiplier()` / `getDllVersions()` 等
    - **dev/release DLL switch**:Debug build 自动用 `nvsdk_ngx_d_dbg.lib`(链入 dev DLL),Release 用 `nvsdk_ngx_d.lib`(release DLL)— 由 CMake 在 T3 配置好
    - DLL 版本运行期查询:用 Windows `GetFileVersionInfo` API 读 `nvngx_dlss.dll` 与 `nvngx_dlssg.dll` 的实际加载版本(可能驱动注入了新版),写到 banner
  - 写 Catch2 `tests/test_ngx_init.cpp`:
    - 真 GPU 上跑(标记为 `[gpu]` tag,CI skip)
    - init NgxContext → SR / FG available + DLL version banner 打印
    - shutdown 干净,二次 init OK
    - 检查 `m_fgMaxMultiplier ≥ 2`(假设测试机是 RTX 40)
  - **每个 NVSDK_NGX_* 调用旁加 `// DLSS §X.X` 引用注释**(per Must Have)

  **Must NOT do**:
  - 不要在 production 代码用 `NVSDK_NGX_GetParameters`(deprecated,用 `GetCapabilityParameters`)
  - 不要 silent fallback(NGX init 失败必须 LOG_ERROR + 抛或返回 expected error)
  - 不要硬编码 application_id(用 with_ProjectID 路径)
  - 不要在 dtor 抛异常
  - 不要忽略 dev DLL 警告(若用户在 Release build 误链 dev DLL,必须 LOG_WARN)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: NGX 各种边界条件 + 版本查询 + DLL 切换,容易写出 silent fail
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 24, 27, 31, 36, 37, 38
  - **Blocked By**: 17

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.5(Init)、§4(DLL loading / version)、§5(Capability query)
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §1 line 429(MultiFrameCountMax 解释)
  - `DLSS/include/nvsdk_ngx_vk.h` API 签名
  - **WHY**:NGX init 是一切的入口,版本 banner 让 release 时可追溯;dev/release DLL switch 防止生产环境 leak overlay

  **Acceptance Criteria**:
  - [ ] NgxContext 在 RTX 40 测试机 init 成功 + LOG_INFO banner 打印
  - [ ] SR available, FG available with max ≥ 2x
  - [ ] 每个 `NVSDK_NGX_*` 调用旁有 `// DLSS §X.X` 引用注释(grep `grep -E "NVSDK_NGX_VULKAN.*\(" src/dlss/ | wc -l` 与 `grep -B1 "NVSDK_NGX_VULKAN.*\(" src/dlss/ | grep -c "// DLSS §"` 应相等)
  - [ ] DLL 版本字符串非空(`getDllVersions()` 返回包含 "3.10" 或 "310.6")
  - [ ] Debug build 链 `_dbg.lib`,Release 链非 dbg

  **QA Scenarios**:
  ```
  Scenario: NGX init 在 RTX 40 成功(happy [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. NgxContext ctx;
      2. assert ctx.isSrAvailable()
      3. assert ctx.isFgAvailable()
      4. assert ctx.getFgMaxMultiplier() >= 2
      5. assert ctx.getDllVersions().sr.find("3.10") != npos
    Evidence: .omo/evidence/task-18-ngx-init.txt + banner.log

  Scenario: 无 GPU graceful(failure)
    Tool: Catch2 (CI runner without NVIDIA)
    Steps:
      1. mock NVSDK_NGX_VULKAN_Init 返回 NVSDK_NGX_Result_FAIL_RHIDeviceNotMet
      2. NgxContext 应抛 std::runtime_error("NGX unavailable: ...")
      3. stderr 含明确错误信息
    Evidence: .omo/evidence/task-18-ngx-no-gpu.txt

  Scenario: 二次 init 干净(stress)
    Tool: Catch2 [gpu]
    Steps:
      1. for i=0..4: { NgxContext ctx; ctx.use(); /* dtor */ }
      2. 无 leak,无重复 banner 打印
    Evidence: .omo/evidence/task-18-ngx-reinit.txt

  Scenario: 文档引用注释完整性(failure prevention)
    Tool: Bash + grep
    Steps:
      1. python scripts/verify_ngx_doc_refs.py src/dlss/  (在 T18 task 中加这个 helper script)
      2. 解析:每个 NVSDK_NGX_* 调用前 N 行内必含 `// DLSS §`
      3. 漏掉的报 file:line
    Evidence: .omo/evidence/task-18-doc-refs.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): NGX init wrapper with capability query + version banner + dev/release DLL switch`
  - Files: `src/dlss/ngx_wrapper.h`、`ngx_wrapper.cpp`、`tests/test_ngx_init.cpp`、`scripts/verify_ngx_doc_refs.py`

- [x] 19. TexturePool + uploadBatch(单 cmdbuf 单 submit 多 texture)

  **What to do**:
  - 写 `src/gpu/texture_pool.h` + `texture_pool.cpp`:
    - `class TexturePool`:
      - 持 `VulkanContext*`
      - 池化 VkImage:`acquire(width, height, format, usage) → PooledTextureHandle`,`release(handle)`(归还到 pool)
      - **关键**:同 (w,h,format,usage) 的 image 复用,避免每帧 vkCreateImage / vkDestroyImage
      - 实现 LRU eviction:超过 budget(默认 4 GB,可配)时 evict 最久未用的
      - 暴露 `getStatistics()`:总分配字节、池中 idle 数、命中率
    - `class UploadBatch`:
      - `addUpload(PooledTextureHandle dst, const void* src, size_t size)` 收集多个 upload
      - `submit(VkQueue transferQueue)`:**单 command buffer + 单 vkQueueSubmit + 单 vkQueueWaitIdle** 处理所有 upload(per Metis 性能建议)
      - 内部用一个 staging buffer pool(也池化)
    - 数据流:CPU staging buffer → GPU → barrier transfer → barrier compute(用 sync2 pipeline barriers)
  - 写 Catch2 `tests/test_texture_pool.cpp`:
    - acquire 100 个 same-shape texture,release 后再 acquire,命中率 100%
    - acquire 不同 shape,验证不复用
    - LRU eviction:超 budget 后旧的被 evict
    - uploadBatch 100 个 32x32 texture → 1 个 vkQueueSubmit 调用(用 mock vkQueueSubmit 计数)
  - 性能基准:对比"naive 100 次 single upload" vs "uploadBatch":batch 至少快 5x(实际机测,不强制)

  **Must NOT do**:
  - **不要每次 acquire 都 vkCreateImage**(必须池化复用)
  - **不要每个 upload 一次 vkQueueSubmit**(必须 batch — v1 这个 bug 是 audit 标的)
  - 不要忘记 sync2 barrier(transfer → compute access mask)
  - 不要在 acquire 时不检查格式兼容(format mismatch 直接抛错)
  - 不要让 TexturePool 跨线程不安全(必须 mutex 保护内部 free list)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Vulkan 资源管理 + 命令缓冲合批,逻辑量大但模式清晰
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 24, 27, 36, 37, 38
  - **Blocked By**: 17

  **References**:
  - v1 `src/gpu/texture_pool.h/.cpp`(已实现但 SR 路径未用 — v2 重写并默认启用)
  - VMA `vmaCreateImage` + `vmaDestroyImage`
  - Vulkan sync2 docs
  - **WHY**:v1 audit 明确指出"批量上传未实现"+ "TexturePool 跨帧未复用",v2 直接实现并强制使用

  **Acceptance Criteria**:
  - [ ] `TexturePool::acquire` 二次同 shape 命中率 == 100%
  - [ ] uploadBatch 内部 vkQueueSubmit 调用次数 == 1(mock count)
  - [ ] LRU eviction 超 budget 后 idle pool 字节 ≤ budget
  - [ ] 100 个 32×32×4B image upload,batch 比 single 快至少 3x(本地 benchmark,可 best-effort)

  **QA Scenarios**:
  ```
  Scenario: Pool 复用率(happy)
    Tool: Catch2
    Steps:
      1. acquire(1920, 1080, R16G16B16A16F, sampled) × 1
      2. release
      3. acquire(...) again → 应返回同一 VkImage(指针比较)
    Evidence: .omo/evidence/task-19-pool-reuse.txt

  Scenario: uploadBatch 单 submit(happy)
    Tool: Catch2 + vkQueueSubmit mock
    Steps:
      1. UploadBatch batch
      2. for i in 0..50: batch.addUpload(...)
      3. batch.submit() → mock 记录 vkQueueSubmit 调用次数 == 1
    Evidence: .omo/evidence/task-19-batch-submit.txt

  Scenario: LRU eviction(happy edge)
    Tool: Catch2
    Steps:
      1. budget = 256 MB
      2. acquire 200 个 1080p 32-bit image(每个 ~32 MB)
      3. 验证 evict 启动,pool size <= budget
      4. 最早 acquired 的 handle 已失效
    Evidence: .omo/evidence/task-19-lru-evict.txt

  Scenario: 不复用不同 format(failure prevention)
    Tool: Catch2
    Steps:
      1. acquire(R16G16B16A16F)
      2. release
      3. acquire(R32_FLOAT) → 必须新创建,不复用
    Evidence: .omo/evidence/task-19-format-isolation.txt

  Scenario: 单 vkQueueSubmit 检测(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'vkQueueSubmit($$$)' src/gpu/texture_pool.cpp → 应仅 1 处(在 UploadBatch::submit 内)
    Evidence: .omo/evidence/task-19-single-submit-site.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): TexturePool with LRU + UploadBatch single-submit pattern`
  - Files: `src/gpu/texture_pool.h`、`texture_pool.cpp`、`tests/test_texture_pool.cpp`

- [x] 20. FramePrefetcher(async EXR read)

  **What to do**:
  - 写 `src/io/frame_prefetcher.h` + `frame_prefetcher.cpp`:
    - `class FramePrefetcher`:
      - 配:`std::vector<std::filesystem::path> frame_paths`(按帧顺序)+ `int prefetch_depth`(默认 3)
      - 内部 `std::thread` worker:循环 `decode(path)` 并 push 到 lock-free SPSC queue
      - 提供 `getNext() → std::expected<DecodedFrame, ReadError>` 阻塞式读出
      - `DecodedFrame { Combined: vector<float>, Depth: vector<float>, Vector: vector<float>, width, height }`(预解析 RGBA + Z + WXYZ vector)
    - 用 OpenEXR `Imf::InputFile` 读 channels;**多通道 EXR 一次 file open + 一次 readPixels**(不要 channel-by-channel 重复 IO)
    - **错误处理**:文件不存在 / 损坏 / channel 缺失 → push 一个 ReadError 到 queue,worker 不崩,主线程拉到 ReadError 再决定 abort
    - **帧 gap 检测**:filename pattern 解析后,if frame_paths[i+1]_index - frame_paths[i]_index > 1 → 标记 `has_gap_after = true`(供 T36 reset 逻辑用)
  - 写 Catch2 `tests/test_frame_prefetcher.cpp`:
    - 给 5 个 fixture EXR(简单合成),prefetch 3 → 拉 5 次 OK
    - 第 3 个文件不存在 → 拉到 ReadError code=FileNotFound,但 prefetcher 不崩
    - benchmark:5 个 4K EXR 顺序读 vs prefetch:prefetch 应快至少 30%(若磁盘是 SSD 应更明显)

  **Must NOT do**:
  - 不要每次 getNext 都 file open(必须 worker 后台预读)
  - 不要在 worker 抛异常(swallow → 推 ReadError)
  - 不要让 prefetch_depth = 0(无效配置,enforce ≥ 1)
  - 不要做 GPU 上传(纯 CPU IO + 解析,GPU upload 在 T19 batch 里)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 多线程 + IO + lock-free queue + 错误传播,中等复杂度
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 36, 37, 38
  - **Blocked By**: 3, 5

  **References**:
  - OpenEXR `IlmImf` docs
  - C++20 `std::jthread` for worker
  - moodycamel ConcurrentQueue 或自写 SPSC ring(如果不引新依赖)
  - **WHY**:管线吞吐瓶颈是磁盘 IO,prefetch 让 GPU 不空等;v1 已实现可复用思路但 v2 重写

  **Acceptance Criteria**:
  - [ ] 5 帧 fixture EXR 全部 getNext 成功,RGBA / Z / Vector 全部解析
  - [ ] 缺帧 → ReadError code 精确,prefetcher 继续工作
  - [ ] benchmark 比 sync 读快 ≥ 30%(本地 best-effort)
  - [ ] worker 能优雅关闭(析构等待 join)

  **QA Scenarios**:
  ```
  Scenario: 5 帧顺序读(happy)
    Tool: Catch2
    Steps:
      1. FramePrefetcher pf(5 path 列表, depth=3)
      2. for i in 0..4: pf.getNext() 成功
      3. 验证 RGBA / Z / Vector 数据维度 + 不全 0
    Evidence: .omo/evidence/task-20-5frame-read.txt

  Scenario: 缺帧 graceful(failure)
    Tool: Catch2
    Steps:
      1. 5 path 列表,中间删掉第 3 个文件
      2. pf.getNext() × 5: 第 3 次返回 ReadError{FileNotFound, "path"}
      3. 第 4、5 次仍 OK(prefetcher 继续)
    Evidence: .omo/evidence/task-20-missing-frame.txt

  Scenario: 帧 gap 检测(happy edge)
    Tool: Catch2
    Steps:
      1. 路径列表 [0001, 0002, 0005, 0006](0003-0004 缺失)
      2. DecodedFrame after 0002: has_gap_after == true
      3. 其他都 false
    Evidence: .omo/evidence/task-20-gap-detect.txt

  Scenario: 损坏 EXR graceful(failure)
    Tool: Catch2
    Steps:
      1. 故意 truncate fixture 第 4 个 EXR
      2. getNext 第 4 次 → ReadError{ParseFailed}
      3. prefetcher worker 不崩
    Evidence: .omo/evidence/task-20-corrupt-exr.txt
  ```

  **Commit**: YES
  - Message: `feat(io): async EXR FramePrefetcher with gap detection`
  - Files: `src/io/frame_prefetcher.h`、`frame_prefetcher.cpp`、`tests/test_frame_prefetcher.cpp`

- [x] 21. AOV channel mapper

  **What to do**:
  - 写 `src/io/channel_mapper.h` + `channel_mapper.cpp`:
    - `class ChannelMapper`:
      - 输入:`DecodedFrame`(T20 prefetcher 给的 RGBA/Z/Vector raw float arrays)
      - 输出:GPU-ready buffers via 3 个 raw byte spans:
        - `SceneColor`(R16G16B16A16_SFLOAT,scene-linear,from RGBA — 需 fp32 → fp16 转换)
        - `Depth`(R32_SFLOAT,from Z channel,**注意 Blender Z 是世界空间线性距离,不是 NDC**;此 task 仅 packing 不转换,T22 做 NDC 转换)
        - `MotionVector`(R16G16B16A16_SFLOAT 4 通道 packed Blender Vector.WXYZ,T23 在 GPU 端做映射)
    - **不在此 task 做 NDC 转换或 MV 映射**(留给 T22/T23 compute shader)
    - 处理 channel 缺失:Z 缺失 → ReadError{MissingDepthChannel}
  - 写 Catch2 `tests/test_channel_mapper.cpp`:
    - mock DecodedFrame 输入 → 输出 buffer 对齐 + size 正确
    - 缺 Z channel → 报错
    - fp32 → fp16 精度损失在 SceneColor 上验证(< 0.001 误差)

  **Must NOT do**:
  - 不要在此 task 做 NDC 转换(违反单一职责)
  - 不要做 MV 符号翻转(留给 T23)
  - 不要假设 EXR 通道顺序(用 OpenEXR API 按名查找)
  - 不要 silent skip 缺通道(必须报错)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: float 转换 + buffer packing,模板化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 22, 23, 36, 37, 38
  - **Blocked By**: 5, 20

  **References**:
  - Blender Cycles AOV pass docs(Z = world-linear distance, Vector = 4ch WXYZ)
  - OpenEXR channel name conventions
  - **WHY**:此 task 是 IO → GPU 边界,严格 packing 防 GPU shader 接错数据

  **Acceptance Criteria**:
  - [ ] SceneColor buffer size == width × height × 4 × 2(R16G16B16A16)
  - [ ] Depth buffer size == width × height × 4(R32)
  - [ ] MV buffer size == width × height × 4 × 2(RG packed 4ch fp16,WXYZ 全保留)
  - [ ] Z 缺失测试报错 MissingDepthChannel
  - [ ] fp32 → fp16 测试,样本像素差异 < 1e-3

  **QA Scenarios**:
  ```
  Scenario: 4K 帧打包(happy)
    Tool: Catch2
    Steps:
      1. mock DecodedFrame 3840×2160 RGBA + Z + Vector
      2. ChannelMapper::map() → 3 buffers
      3. 验证 size + 中心像素值 round-trip(fp32→fp16→fp32 误差 < 1e-3)
    Evidence: .omo/evidence/task-21-pack-4k.txt

  Scenario: 缺通道报错(failure)
    Tool: Catch2
    Steps:
      1. mock DecodedFrame 无 Z
      2. ChannelMapper::map() → ReadError{MissingDepthChannel, "channel 'Z' not found"}
    Evidence: .omo/evidence/task-21-missing-channel.txt

  Scenario: 4 通道全保留(failure prevention)
    Tool: Catch2
    Steps:
      1. mock Vector 4 通道 (1, -2, 3, -4)
      2. 输出 MV buffer 中心 R/G/B/A 解码后 == (1, -2, 3, -4)(允许 fp16 精度)
    Evidence: .omo/evidence/task-21-vector-4ch-preserved.txt
  ```

  **Commit**: YES
  - Message: `feat(io): channel mapper Combined/Depth/Vector → GPU-ready packed buffers`
  - Files: `src/io/channel_mapper.h`、`channel_mapper.cpp`、`tests/test_channel_mapper.cpp`

- [x] 22. Depth 转换 compute shader(world-linear → NDC [0,1])

  **What to do**:
  - 写 `src/shaders/depth_world_to_ndc.comp`(GLSL compute shader):
    - 输入:`layout(set=0, binding=0) uniform sampler2D srcDepth` + `layout(set=0, binding=1, r32f) uniform image2D dstDepth`
    - Push constants:`mat4 projection`(per-frame,from dlssconfig)
    - 算法:对每像素读 world-linear depth Z_world,经投影矩阵转换到 NDC `[0, 1]` 范围
      - 可选两种实现:
        - (a) 用 `clip = projection * vec4(0, 0, -Z_world, 1.0)`;`ndc.z = clip.z / clip.w`(标准透视投影路径)
        - (b) 简化:`ndc.z = (Z_world - near) / (far - near)`(线性 depth,不准但简单)
      - **采用 (a)**(per DLSS doc §3.5,期望 NDC 而非线性)
    - 注释:`// DLSS SR §3.5 - depth in NDC [0, 1] range, non-inverted`
  - 写 `src/gpu/passes/depth_pass.h` + `depth_pass.cpp`:`class DepthConversionPass` 封装 shader 调度
  - Catch2 `tests/test_depth_pass.cpp`:
    - 构造已知 projection matrix 与已知 depth 输入(near=0.1, far=100, depth=10)
    - 跑 shader,readback NDC depth
    - 与 CPU 端同公式计算结果对比 ε < 1e-4

  **Must NOT do**:
  - 不要在 shader 用线性深度近似(必须用 projection matrix)
  - **不要假设 reverse-Z**(Blender NDC 是 0=near, 1=far,与 UE 反)
  - 不要在 shader 里硬编码 near/far(从 push constant 或 projection matrix 提取)
  - 不要忘 image layout transition(input sampler vs output storage)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: 单 compute shader + 简单封装
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 24, 27, 36, 37, 38
  - **Blocked By**: 3(glslc), 17, 19

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.5(depth NDC requirement)
  - Blender Cycles Z pass:world-linear distance(已确认)
  - **WHY**:DLSS 期望 NDC depth,Blender 提供 linear distance,必须 GPU 端转换

  **Acceptance Criteria**:
  - [ ] shader 编译为 SPIR-V
  - [ ] 测试 input depth=10, near=0.1, far=100, projection 标准透视:输出 NDC ≈ 公式计算值,ε < 1e-4
  - [ ] DLSS 文档引用注释存在
  - [ ] 多个 depth 像素并行测试,无 race condition

  **QA Scenarios**:
  ```
  Scenario: NDC 转换正确(happy)
    Tool: Catch2 + GPU readback
    Steps:
      1. perspective(fov=60°, aspect=1, near=0.1, far=100)
      2. depth values [0.1, 1.0, 10.0, 50.0, 100.0]
      3. shader 跑 → readback NDC values
      4. 与 CPU 同矩阵计算对比 ε < 1e-4
    Evidence: .omo/evidence/task-22-ndc-correct.txt

  Scenario: 不假设 reverse-Z(failure prevention)
    Tool: grep
    Steps:
      1. grep -E "reverse|inverted|1\.0 - depth" src/shaders/depth_world_to_ndc.comp → 0 匹配
    Evidence: .omo/evidence/task-22-no-reverse-z.txt

  Scenario: 文档引用存在(failure prevention)
    Tool: grep
    Steps:
      1. grep "DLSS SR §3" src/shaders/depth_world_to_ndc.comp → ≥1 匹配
    Evidence: .omo/evidence/task-22-doc-ref.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): depth world-linear→NDC compute shader`
  - Files: `src/shaders/depth_world_to_ndc.comp`、`src/gpu/passes/depth_pass.h`、`depth_pass.cpp`、`tests/test_depth_pass.cpp`

- [x] 23. MV 转换 compute shader + 实证 sanity test

  **What to do**:
  - 写 `src/shaders/mv_blender_to_dlss.comp`(GLSL compute):
    - 输入:`layout(set=0, binding=0) uniform sampler2D srcMV4ch`(Blender Vector pass packed RGBA)
    - 输出:`layout(set=0, binding=1, rg16f) uniform image2D dstMV2ch`
    - 算法:`vec4 blender = texture(srcMV4ch, uv); vec2 dlss = vec2(blender.x, -blender.y); imageStore(dstMV2ch, ivec2(gl_GlobalInvocationID.xy), vec4(dlss, 0, 0));`
    - 注释:
      ```
      // DLSS SR §3.6 - per-pixel motion vectors curr→prev in pixel-units, X positive = right, Y positive = down
      // Blender Vector pass (verified empirically per blender/tests/MV_SANITY_RESULT.md, T13):
      //   Vector.X = prev→curr X displacement, X positive = LEFT
      //   Vector.Y = prev→curr Y displacement, Y positive = DOWN
      //   Vector.Z, Vector.W = curr→next (unused)
      // Mapping: DLSS_MV = (Blender.X, -Blender.Y)
      //   X axis: Blender X 正=左 + DLSS X 正=右 + 时间反转 → 两次符号反转抵消 → 直通
      //   Y axis: Blender Y 正=下 + DLSS Y 正=下 + 时间反转 → 仅时间反转 → 取反
      ```
  - 写 `src/gpu/passes/mv_pass.h` + `mv_pass.cpp`
  - **关键 sanity test**`tests/test_mv_conversion_sanity.cpp`:
    - 读 `blender/tests/MV_SANITY_RESULT.md`(T13 产出),提取实证测试场景的 plane 屏幕位移
    - 构造对应的 mock 输入 4ch Blender Vector buffer
    - 跑 mv shader → readback 2ch DLSS MV
    - 断言:中心像素 DLSS MV 符号与 "(Blender.X, -Blender.Y) 推导后 curr→prev 期望" 完全一致
    - **如果 T13 sanity test 失败标记了不同符号映射**(在 MV_SANITY_RESULT.md 中),T23 必须按那份结论修改 shader 公式 + 更新注释
  - **失败处理**:`tests/test_mv_conversion_sanity.cpp` 直接 fail 时,T23 hard-block Wave 4,直到映射统一

  **Must NOT do**:
  - **不要写 `vec2 dlss = -blender.xy`(全轴取反)** — 那是 v1 audit 的错误模式
  - **不要假设 Blender Vector 是 prev→curr 还是 curr→prev** — 用 T13 实证结论
  - 不要在 shader 里做 MVScale 缩放(per DLSS doc,scale 由 NGX `InMVScale` 参数控制)
  - 不要忘记 4ch → 2ch 转换前不要 sample Vector.Z/W(那是 curr→next 数据)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 符号约定 + 实证验证 + v1 历史 bug 防回归;此 task 错则 SR 全废
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 3
  - **Blocks**: 24, 27, 36, 37, 38
  - **Blocked By**: 3, 17, 19, 21, **必须依赖 T13 的 MV_SANITY_RESULT.md**

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.6(MV format requirement)
  - `blender/tests/MV_SANITY_RESULT.md`(T13 实证结论)
  - 用户提供的 Blender StackExchange 答案(quoted in MV_SANITY_RESULT.md)
  - `.sisyphus/sr-fg-audit-2026-05-29.md`(v1 错误案例 — `-blenderMv` 双轴取反)
  - **WHY**:MV 错位 1 像素就 SR 退化;v1 因为没实证导致这个 bug 持续数月;v2 用实证测试钉死结论

  **Acceptance Criteria**:
  - [ ] shader 编译为 SPIR-V
  - [ ] sanity test 通过:输入对应 plane 屏幕右移 + 下移的 Blender 4ch MV → 输出 2ch DLSS MV 符号正确
  - [ ] shader 注释含完整 mapping 推导 + T13 实证引用
  - [ ] grep 全 codebase 无 `-blenderMv` 全轴取反模式
  - [ ] 与 T13 的 MV_SANITY_RESULT.md 内符号结论一致(若 T13 实证发现不同 mapping,T23 必须采用并更新)

  **QA Scenarios**:
  ```
  Scenario: 实证映射验证(happy critical)
    Tool: Catch2 + GPU
    Steps:
      1. 读 MV_SANITY_RESULT.md 提取实证 case:plane 右移 5px + 下移 3px,期望 DLSS_MV = (-5, -3)
      2. 构造 mock Blender 4ch:Vector.X=-5(右移因 X 正=左), Vector.Y=+3(下移)
      3. 跑 mv_pass shader
      4. readback DLSS 2ch:中心像素 (-5, -3) ε < 0.5(fp16)
    Expected Result: 完全匹配
    Evidence: .omo/evidence/task-23-mv-empirical.txt

  Scenario: 不能用 -blenderMv 全轴取反(failure prevention)
    Tool: grep + ast_grep
    Steps:
      1. grep -E "= *-blender.xy|= *-blender\.X|negate.*both" src/shaders/ → 0 匹配
      2. ast_grep --lang glsl '-srcMV4ch.xy' → 0 匹配
    Expected Result: 双轴取反模式完全不存在
    Evidence: .omo/evidence/task-23-no-double-negate.txt

  Scenario: MV_SANITY_RESULT.md 必须存在(blocking)
    Tool: Bash
    Steps:
      1. test -f blender/tests/MV_SANITY_RESULT.md
      2. cat blender/tests/MV_SANITY_RESULT.md | grep -E "VERDICT|MAPPING" → 至少 1 匹配
    Expected Result: 文件存在 + 含明确结论
    Evidence: .omo/evidence/task-23-blocking-dep.txt

  Scenario: 文档引用注释完整性
    Tool: grep
    Steps:
      1. grep -c "DLSS SR §3" src/shaders/mv_blender_to_dlss.comp → ≥ 1
      2. grep "MV_SANITY_RESULT" src/shaders/mv_blender_to_dlss.comp → 1
    Evidence: .omo/evidence/task-23-doc-refs.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): MV Blender(4ch)→DLSS(2ch) shader anchored on T13 empirical mapping`
  - Files: `src/shaders/mv_blender_to_dlss.comp`、`src/gpu/passes/mv_pass.h`、`mv_pass.cpp`、`tests/test_mv_conversion_sanity.cpp`

- [x] 24. Stage 2 — DLSS-SR Eval 封装(NGX-direct)

  **What to do**:
  - 写 `src/dlss/dlss_sr.h` + `dlss_sr.cpp`:
    - `class DlssSrPass`:
      - ctor: 持 `NgxContext*` + `VulkanContext*` + `TexturePool*`
      - `void create(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, QualityMode q)`:
        ```cpp
        // DLSS SR §3.5 - Feature creation
        NVSDK_NGX_DLSS_Create_Params p = {};
        p.Feature.InWidth = inW;
        p.Feature.InHeight = inH;
        p.Feature.InTargetWidth = outW;
        p.Feature.InTargetHeight = outH;
        p.Feature.InPerfQualityValue = toNgxQuality(q);  // DLAA → DLAA, etc.
        // DLSS SR §3.6.2 + audit confirms - MV at render res
        p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                               | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                               | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        // DLSS SR §3.5 - DepthInverted NOT set (Blender NDC non-inverted)
        // DLSS SR §3.11 - InSharpness deprecated, do NOT set
        NGX_VULKAN_CREATE_DLSS_EXT(cmdBuf, 1, 1, &m_handle, m_ngxParams, &p);
        // DLSS SR §3.12 - per-quality preset
        setPresetForQuality(m_ngxParams, q);  // DLAA/Q/B/UQ→K, P→M, UP→L
        ```
      - `void evaluate(VkCommandBuffer cmd, const SrInputs& in)`:
        ```cpp
        // DLSS SR §3.6 - per-frame eval params
        NVSDK_NGX_VK_DLSS_Eval_Params p = {};
        p.Feature.pInColor = wrapVk(in.scene_color_render_res);
        p.Feature.pInOutput = wrapVk(in.scene_color_output_res);
        p.pInDepth = wrapVk(in.depth_ndc);              // DLSS SR §3.5
        p.pInMotionVectors = wrapVk(in.motion_vec_dlss); // DLSS SR §3.6 - curr→prev px units
        p.InColorSubrectBase = {0, 0};
        p.InDepthSubrectBase = {0, 0};
        p.InMVSubrectBase = {0, 0};
        p.InOutputSubrectBase = {0, 0};
        p.InRenderSubrectDimensions = {in.render_w, in.render_h};
        // DLSS SR §3.10 - auto exposure: pInExposureTexture = nullptr
        p.pInExposureTexture = nullptr;
        p.InPreExposure = 1.0f;
        p.InExposureScale = 1.0f;
        // DLSS SR §3.7 - jitter pixel space [-0.5, +0.5]
        p.InJitterOffsetX = in.jitter_x;
        p.InJitterOffsetY = in.jitter_y;
        // DLSS SR §3.6.3 - MV in pixel units, no rescale needed
        p.InMVScaleX = 1.0f;
        p.InMVScaleY = 1.0f;
        // DLSS SR §3.13 - reset on first frame, gap, or camera cut
        p.InReset = in.reset ? 1 : 0;
        // DLSS SR §3.14 - frame time
        p.InFrameTimeDeltaInMsec = 1000.0f / in.fps;  // NEVER hardcode 33.3f
        p.pInBiasCurrentColorMask = nullptr;  // optional
        NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, m_handle, m_ngxParams, &p);
        ```
      - `void destroy()`:`NVSDK_NGX_VULKAN_ReleaseFeature(m_handle)`
    - `struct SrInputs { VkImageView scene_color_render_res; VkImageView scene_color_output_res; VkImageView depth_ndc; VkImageView motion_vec_dlss; uint32_t render_w; uint32_t render_h; float jitter_x; float jitter_y; bool reset; int fps; }`
    - `static int toNgxQuality(QualityMode q)`:DLAA→DLAA, UltraQuality→MaxQuality, ..., per UE 映射
    - `static void setPresetForQuality(NGXParams* p, QualityMode q)`:per §3.12 推荐表(DLAA/Q/B/UQ→K, P→M, UP→L)
  - **每个 NVSDK_NGX_* 行都加 `// DLSS §X.X` 注释**(per Must Have)
  - 写 Catch2 `tests/test_dlss_sr.cpp`(`[gpu]` tag):
    - 用 `static_camera.json`(T1 cherry-picked fixture)+ 5 帧 1080p → 2160p
    - SR Eval 5 帧成功(无 NGX error)
    - 第 1 帧 reset=true,2-5 帧 reset=false
    - 像素 hash 第 1 帧 vs 第 5 帧 → 不同(因 history 累积),但 fp 范围合理

  **Must NOT do**:
  - **不要设 InSharpness**(SR §3.11 已废弃)
  - **不要设 DepthInverted flag**(Blender NDC 非反转)
  - **不要硬编码 InFrameTimeDeltaInMsec = 33.3f**
  - **不要每帧 reset=true**
  - **不要 InMVScaleX/Y = anything other than 1.0f**(audit 强调)
  - **不要复制 UE bUseBiasCurrentColorMask**(那是 UE 特殊优化,我们不做)
  - 不要在 evaluate 内 vkCreateImage(image 由 caller 通过 TexturePool 提供)
  - **每个 NVSDK_NGX_* 调用旁如果无 `// DLSS §` 注释 → F1 reviewer 会拒**

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: NGX 参数极易写错 + UE 抄袭风险 + 文档引用强制 + 历史 bug 高发区
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4
  - **Blocks**: 36, 38(SR-only / SR+FG 主流程)
  - **Blocked By**: 5, 6, 17, 18, 19, 22, 23

  **References**:
  - **DLSS doc 必读章节**:
    - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.5(Create + Init)
    - §3.6(MV format)、§3.6.2(MVLowRes)、§3.6.3(MVScale=1)
    - §3.7(Jitter pixel space)
    - §3.10(AutoExposure)
    - §3.11(InSharpness deprecated)
    - §3.12(Per-quality preset K/L/M)
    - §3.13(Reset)
    - §3.14(InFrameTimeDeltaInMsec)
  - UE 参考(仅 cross-check):`Plugins\DLSS\Source\NGXVulkanRHI\Private\NGXVulkanRHI.cpp:411-470`
  - **WHY**:此 task 是 SR 落地,所有 audit 列出的 bug 在此回归;每个参数赋值必须有 doc 引用注释,F1 reviewer 会逐行 grep

  **Acceptance Criteria**:
  - [ ] DlssSrPass create + evaluate 5 帧成功(GPU 测试)
  - [ ] grep `// DLSS §` 在 dlss_sr.cpp 中 ≥ 12 行(每个 NGX 参数赋值前都有)
  - [ ] grep `InMVScaleX.*=.*1\.0f` + `InMVScaleY.*=.*1\.0f` 各 1 处(精确值)
  - [ ] grep `InSharpness` → 0 匹配
  - [ ] grep `DepthInverted` → 0 匹配
  - [ ] grep `33\.3f` → 0 匹配
  - [ ] grep `InReset.*=.*1.*for.*every|true.*always` → 0 匹配
  - [ ] reset 状态测试:首帧 reset=1,2-5 帧 reset=0
  - [ ] preset 映射:Performance → preset M(用 mock NgxParams 验证 SetUI 调用)

  **QA Scenarios**:
  ```
  Scenario: 5 帧 SR Eval 成功(happy [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. 加载 fixture 5 帧 1080p RGBA + Z + Vector(用 channel mapper + depth/mv shader 处理)
      2. DlssSrPass.create(1080p → 2160p, Performance)
      3. for i in 0..4: pass.evaluate(jitter from Halton, reset = (i==0))
      4. readback 输出 2160p,验证全部像素非 NaN/Inf,中心区像素 fp 范围合理
    Evidence: .omo/evidence/task-24-sr-5frame.txt + screenshot of frame_5.exr

  Scenario: 文档注释完整性(failure prevention)
    Tool: scripts/verify_ngx_doc_refs.py
    Steps:
      1. python scripts/verify_ngx_doc_refs.py src/dlss/dlss_sr.cpp
      2. 每个 NVSDK_NGX_* 调用前 5 行内必含 `// DLSS §`
      3. 任何缺漏 → 报 file:line + fail
    Evidence: .omo/evidence/task-24-doc-ref-complete.txt

  Scenario: v1 audit bugs 不复现(failure prevention)
    Tool: grep
    Steps:
      1. grep -E "InMVScale.*outputWidth|MVScale.*\*.*output" src/dlss/ → 0
      2. grep -E "InMVScale[XY]\s*=\s*[02]\.0f" src/dlss/ → 0(不能等于 0 或 2)
      3. grep "33\.3f" src/dlss/ → 0
      4. grep "InSharpness" src/dlss/ → 0
      5. grep "DepthInverted" src/dlss/ → 0
    Expected Result: 5 grep 全部 0 匹配
    Evidence: .omo/evidence/task-24-no-audit-regressions.txt

  Scenario: Per-quality preset(happy)
    Tool: Catch2 mock
    Steps:
      1. 对 6 quality mode 每个 create(),mock NVSDK_NGX_Parameter_SetUI
      2. 验证 setUI 调用 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_<Mode> 的 value
         - DLAA/Q/B/UQ → K
         - P → M
         - UP → L
    Evidence: .omo/evidence/task-24-preset-mapping.txt

  Scenario: Reset 触发条件(happy edge)
    Tool: Catch2
    Steps:
      1. 序列 [reset=true(首帧), false, false, true(模拟 gap), false]
      2. 验证 NVSDK_NGX_VK_DLSS_Eval_Params.InReset 5 次依次为 1,0,0,1,0
    Evidence: .omo/evidence/task-24-reset-pattern.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): SR Eval pass with strict §-citation comments + audit-regression guards`
  - Files: `src/dlss/dlss_sr.h`、`dlss_sr.cpp`、`tests/test_dlss_sr.cpp`、`scripts/verify_ngx_doc_refs.py`(若 T18 未建则此 task 加)

- ~~25.~~ **REMOVED** — CAS sharpen compute shader 删除(per 用户决议 2026-06-02 round 2):DLSS SR §3.11 原文 "The deep learning model used in DLSS is **trained to produce sharp images**",外部锐化仅作为"想再锐一点"的开发者 escape hatch,v2 默认管线不需要。`src/shaders/cas_sharpen.comp`、`src/gpu/passes/cas_pass.*`、`tests/test_cas_pass.cpp` **不实现**;CLI 无 `--sharpness` flag。任务编号 25 留 gap(F4 reviewer 不验证此号位)。如未来需求重启,从此 commit history 复活。


- [x] 26. Stage 4 — Tonemap + EOTF compute shader(HDR ACES Filmic + LDR no-tonemap)

  **What to do**:
  - 写 `src/shaders/tonemap_eotf.comp`(GLSL compute,push constant `int mode`):
    - **`mode = 0` LDR path**(`is_hdr=false`):
      - **不做 tonemap**(per 用户 round 2 决议)
      - clamp scene-linear `[0, 1]`(显式 saturate;values > 1.0 被剪裁,LDR 项目可接受)
      - 应用 sRGB EOTF:`output = (linear < 0.0031308) ? 12.92 × linear : 1.055 × pow(linear, 1/2.4) - 0.055`
    - **`mode = 1` HDR path**(`is_hdr=true`):
      - 应用 **ACES Filmic HDR tonemapper**(per 用户 round 2 决议),公式:
        ```glsl
        // ACES Filmic HDR tonemap (Krzysztof Narkowicz approximation, monotonic + invertible)
        // Maps scene-linear [0, infinity) → [0, 1] with HDR-aware shoulder
        // Reference: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        vec3 tonemapped = saturate((scene_linear * (a * scene_linear + b)) /
                                   (scene_linear * (c * scene_linear + d) + e));
        ```
      - 应用 PQ ST.2084 EOTF on tonemapped(假设 1.0 scene-linear = 100 nits diffuse white,per Blender 5.1 docs `displays_views.rst:271-272`):
        ```glsl
        // PQ ST.2084 EOTF (linear → PQ encoded [0,1])
        // Reference: ITU-R BT.2100, SMPTE ST.2084
        const float m1 = 0.1593017578125;
        const float m2 = 78.84375;
        const float c1 = 0.8359375;
        const float c2 = 18.8515625;
        const float c3 = 18.6875;
        // Blender 1.0 scene-linear = 100 nits → 0.01 PQ-normalized
        vec3 L = tonemapped * 0.01;  // = nits / 10000
        vec3 Lm1 = pow(L, vec3(m1));
        vec3 pq = pow((c1 + c2 * Lm1) / (1.0 + c3 * Lm1), vec3(m2));
        ```
  - 写 `src/gpu/passes/tonemap_pass.h` + `tonemap_pass.cpp`,根据 `dlssconfig.is_hdr` 选 mode
  - **管线 wiring**:Stage 4 仅在 FG enabled 时调用(SR-only path 完全跳过 Stage 4/5/6)
  - Catch2 测试:
    - LDR: clamp + sRGB:`linear=0.5` → `0.7354`,`linear=2.0`(超界)→ clamp 1.0 → 1.0,`linear=0.0` → 0.0
    - HDR: ACES Filmic + PQ:`linear=0.18`(18% mid-grey)→ ACES 后 ≈ `0.18` → PQ 后 ≈ `0.36`(标准点),`linear=4.0`(亮 16 stops)→ ACES 0.93 → PQ 0.65,`linear=0.0` → 0.0
    - round-trip(经 T28 inverse)误差 < 0.02(ACES inverse 精度有限)

  **Must NOT do**:
  - 不要在 LDR path 加 ACES / Reinhard / Filmic 等 tonemap(per 用户决议:LDR 不走 tonemap)
  - 不要在 HDR path 跳 ACES Filmic 直接 PQ(scene-linear 没有 ACES 压缩 highlight 直接喂 PQ 会丢 HDR 表现)
  - 不要在 SR-only 路径调用 Stage 4(per Must Have)
  - 不要混淆 EOTF 与 OETF
  - 不要把用户 Blender view_transform(AgX/Filmic/ACES 2.0)真的复制到 CLI(per 用户决议:CLI 内部 ACES Filmic 即可,不引 OCIO 重依赖)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: 已知数学公式(ACES Narkowicz + sRGB IEC + PQ ST.2084),GLSL ~50 行
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4
  - **Blocks**: 27(FG 需要 display-ready 输入)、28(逆变换共享公式)、37, 38
  - **Blocked By**: 5, 17

  **References**:
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §HDR L1454-1473(display-ready + tonemap + EOTF requirement)
  - ACES Filmic Narkowicz approximation: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
  - SMPTE ST.2084 PQ 公式
  - sRGB IEC 61966-2-1 公式
  - Blender `displays_views.rst:271-272`(1.0 scene-linear = 100 nits 约定)
  - **WHY**:FG **必须吃** tonemap 后 + EOTF 编码的 display-ready 输入(per FG §HDR);v1 的 "10-nit clamp" 错觉就是因为没 tonemap 直接喂 FG;v2 严格按文档实现

  **Acceptance Criteria**:
  - [ ] LDR mode: 5 个测试点全过(0, 0.0031308, 0.5, 1.0, 2.0(clamp))
  - [ ] HDR mode: 5 个测试点全过(0, 0.18 mid-grey, 1.0 white, 4.0 high luminance, 16.0 extreme)
  - [ ] round-trip 经 T28 inverse 误差 < 0.02

  **QA Scenarios**:
  ```
  Scenario: LDR clamp + sRGB EOTF(happy)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(0.5, 0.7, 2.0) scene-linear
      2. tonemap_pass(mode=LDR)
      3. 输出 R≈0.7354 (sRGB(0.5)), G≈0.8541 (sRGB(0.7)), B==1.0 (clamp 后 sRGB(1.0)) ε < 0.002
    Evidence: .omo/evidence/task-26-ldr-srgb.txt

  Scenario: HDR ACES Filmic + PQ(happy)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(0.18, 0.18, 0.18)(18% mid-grey scene-linear)
      2. tonemap_pass(mode=HDR)
      3. 验证 ACES 中间步骤 ≈ 0.18 ± 0.02(mid-grey 经 ACES 接近 mid-grey)
      4. 验证最终 PQ encoded ≈ 0.36(对应 18 nits in PQ)
    Evidence: .omo/evidence/task-26-hdr-aces-pq.txt

  Scenario: HDR 极端高光不爆(failure prevention)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(100.0, 100.0, 100.0)(模拟太阳级亮度)
      2. tonemap_pass(mode=HDR)
      3. 验证 ACES tonemapped < 1.0(成功压缩)
      4. 验证 PQ encoded ≈ 0.66(对应 ~10000 nits 的 PQ encoding 值)
    Evidence: .omo/evidence/task-26-hdr-highlight.txt

  Scenario: 不允许在 SR-only 路径调用(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep 'TonemapPass::execute($$$)' src/pipeline/sr_only_pipeline.cpp → 0 匹配
    Evidence: .omo/evidence/task-26-sr-only-no-tonemap.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): tonemap+EOTF (LDR sRGB-only / HDR ACES Filmic + PQ ST.2084)`
  - Files: `src/shaders/tonemap_eotf.comp`、`src/gpu/passes/tonemap_pass.h`、`tonemap_pass.cpp`、`tests/test_tonemap_pass.cpp`

- [x] 27. Stage 5 — DLSS-FG Eval 封装(NGX-direct)+ multi-frame 循环

  **What to do**:
  - 写 `src/dlss/dlss_fg.h` + `dlss_fg.cpp`:
    - `class DlssFgPass`:
      - ctor: 持 `NgxContext*` + `VulkanContext*` + `TexturePool*`
      - `void create(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, int multiplier)`:
        ```cpp
        // DLSS FG §5 - Create params
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, outW);                     // FG §5 L459
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, outH);
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSSG_RenderWidth, inW);          // FG §5
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSSG_RenderHeight, inH);
        // FG §5 - HDR backbuffer format (RGB10A2 PQ)
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
                                  VK_FORMAT_A2B10G10R10_UNORM_PACK32);  // FG §HDR L1462
        // FG §1 L518 - multi-frame
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSSG_MultiFrameCountMax,
                                  multiplier - 1);  // 2x → 1, 3x → 2, 4x → 3
        // v310.6.0 - skip optional resources to save VRAM
        uint32_t neverProvided = NVSDK_NGX_DLSSG_ResourceFlags_HUDLess
                               | NVSDK_NGX_DLSSG_ResourceFlags_UI
                               | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha
                               | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
        NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
                                  neverProvided);  // v310.6.0 §nvsdk_ngx_defs_dlssg.h
        NVSDK_NGX_VULKAN_CreateFeature(cmdBuf, NVSDK_NGX_Feature_FrameGeneration,
                                       p, &m_handle);
        ```
      - `void evaluate(VkCommandBuffer cmd, const FgInputs& in)`:
        - 内部循环 `for (int i = 0; i < multiplier - 1; ++i)` // FG §1 L518 multi-frame
        ```cpp
        // FG §6 - per-frame eval
        NVSDK_NGX_VK_DLSSG_Eval_Params p = {};
        p.pBackbuffer = wrapVk(in.backbuffer_display_ready);  // FG §6 L539 - post-tonemap+EOTF
        p.pDepth = wrapVk(in.depth_ndc_render_res);           // FG §6 L545 - same as SR
        p.pMVecs = wrapVk(in.motion_vec_dlss_render_res);     // FG §6 L555 - same as SR
        p.pOutputInterpFrame = wrapVk(in.output_interp[i]);
        // optional all nullptr - we set ResourceNeverProvided in Create
        p.pHudless = nullptr;
        p.pUI = nullptr;
        p.pUIAlpha = nullptr;
        p.pBidirectionalDistortionField = nullptr;
        p.pOutputRealFrame = nullptr;
        p.pOutputDisableInterpolation = nullptr;
        // FG §6 L591-593 - mvec scale = (1, 1) when MVs already in pixel-units
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.0f);
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.0f);
        // FG §6 L587 - reset on cuts
        NVSDK_NGX_Parameter_SetUI(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_Reset, in.reset ? 1 : 0);
        // FG §6 L590 - Blender NDC depth non-inverted
        NVSDK_NGX_Parameter_SetUI(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_DepthInverted, 0);
        // FG §HDR L1506 - HDR (PQ) path
        NVSDK_NGX_Parameter_SetUI(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR,
                                  in.is_hdr ? 1 : 0);
        // FG SDK header - MV doesn't include jitter, includes camera motion
        NVSDK_NGX_Parameter_SetUI(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1);
        NVSDK_NGX_Parameter_SetUI(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_MotionVectorsJittered, 0);
        // FG §6 L595 - clipToPrevClip / prevClipToClip from per-frame camera
        setMatrix4x4(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, in.clip_to_prev_clip);
        setMatrix4x4(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, in.prev_clip_to_clip);
        setMatrix4x4(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, in.view_to_clip);
        // FG §6 L599 - camera basis + near/far/fov/aspect
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraNear, in.cam_near);
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraFar, in.cam_far);
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraFOV, in.fov);
        NVSDK_NGX_Parameter_SetF(m_ngxParams, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio,
                                 in.aspect_ratio);
        // ... CameraPos / CameraUp / CameraRight / CameraFwd 设置
        // FG §1 L518 - multi-frame index
        NVSDK_NGX_Parameter_SetI(m_ngxParams, NVSDK_NGX_Parameter_DLSSG_MultiFrameIndex, i);
        NGX_VULKAN_EVALUATE_DLSSG(cmd, m_handle, m_ngxParams, &p);
        ```
    - `struct FgInputs { backbuffer_display_ready, depth_ndc_render_res, motion_vec_dlss_render_res, output_interp[], view_to_clip, prev_clip_to_clip, clip_to_prev_clip, cam_near, cam_far, fov, aspect_ratio, cam_pos, cam_up, cam_right, cam_fwd, is_hdr, reset, multiplier }`
  - **每个 NVSDK_NGX_* 调用都必须有 `// FG §X.X` 引用注释**(per Must Have)
  - 写 Catch2 `tests/test_dlss_fg.cpp`(`[gpu]` tag):
    - 用 fixture 5 帧 + camera matrices,FG 2x → 9 个输出帧(2N-1)
    - FG 4x → 17 个输出帧(4N-3)— 需要 RTX 40+ 测试机
    - 每个 NVSDK_NGX_* 行附注释 grep 验证

  **Must NOT do**:
  - **不要设 MvecScale = (1/W, 1/H)**(那是 v2-refactor-goals.md §1.5 的错误注释,正确是 (1.0, 1.0))
  - **不要设 pNoPostProcessingColor**(v310.6.0 已删)
  - **不要设 pOutputRealFrame**(live game 用,offline 不需要)
  - **不要在 FG 之前忘记 tonemap+EOTF**(必须 display-ready)
  - **不要传 unjittered camera matrices**(per Must Have:matrices 不含 jitter)
  - **不要忘记 multi-frame 循环**(2x 也是 multiplier-1=1 次 eval,4x 是 3 次)
  - **不要硬编码 multiplier**(从 in.multiplier 读)
  - **不要重新分配 depth/MV image**(per FG §6 L545:必须用 SR 同一份资源)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: FG 比 SR 参数更多 + multi-frame 循环逻辑 + HDR/EOTF 边界 + v310.6.0 新 ResourceFlags
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4
  - **Blocks**: 37, 38
  - **Blocked By**: 5, 6, 17, 18, 19, 22, 23, 26

  **References**:
  - **DLSS FG doc 必读**:
    - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §1(管线 + multi-frame line 518)
    - §5(Create params line 459-464)
    - §6(Eval inputs,depth/MV 与 SR 同一份 line 545)
    - §HDR(EOTF 要求 line 1448-1500)
    - §6 L591-593(mvecScale=1 for pixel-unit MVs)
  - `DLSS/include/nvsdk_ngx_defs_dlssg.h`(v310.6.0,新 ResourceFlags)
  - **WHY**:FG 是 v1 没做对的部分(10-nit clamp 假说),v2 必须严格按文档实现

  **Acceptance Criteria**:
  - [ ] DlssFgPass create + evaluate 5 帧 → 9 输出(2x)+ 17 输出(4x)成功
  - [ ] grep `// FG §` 在 dlss_fg.cpp ≥ 18 行
  - [ ] grep `MvecScaleX.*1\.0f` + `MvecScaleY.*1\.0f` 各 1 处(精确值)
  - [ ] grep `pNoPostProcessingColor` → 0 匹配(v310.6.0 删除)
  - [ ] grep `pOutputRealFrame.*=.*[^nullptr]` → 0 匹配(必须 nullptr)
  - [ ] grep `1\.0[fF]?\s*/\s*\(?in\.render_w` → 0 匹配(不能是 1/W)
  - [ ] grep `ResourceNeverProvided` → 1 匹配(必须设)
  - [ ] multi-frame 测试:multiplier=2 时 evaluate 调用 1 次;=4 时调用 3 次

  **QA Scenarios**:
  ```
  Scenario: 5 帧 FG 2x → 9 输出(happy [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. 加载 fixture 5 帧 1080p backbuffer + depth + MV + camera matrices
      2. DlssFgPass.create(1080p, 1080p, multiplier=2)
      3. for input frame i in 1..4:
         pass.evaluate(backbuffer_i, prev=backbuffer_{i-1}, output_interp[1] alloced)
         产出 1 个 interp 帧
      4. 总输出帧数 = 4 input pairs × 1 interp + 5 real = 9
    Evidence: .omo/evidence/task-27-fg-2x.txt + 9 frames

  Scenario: FG 4x multi-eval(happy [gpu RTX40])
    Tool: Catch2 [gpu]
    Steps:
      1. multiplier=4
      2. for each input pair: 调 evaluate 3 次(MultiFrameIndex 0,1,2)
      3. 总输出 = 4×N-3 = 17(其中 5 real + 12 interp)— N=5 input,3 interp/pair × 4 pairs = 12 interp + 5 real = 17
    Expected Result: 17 个输出帧
    Evidence: .omo/evidence/task-27-fg-4x.txt

  Scenario: 关键参数 grep 防回归(failure prevention)
    Tool: grep
    Steps:
      1. grep "MvecScaleX.*=.*1\.0" src/dlss/dlss_fg.cpp → ≥1
      2. grep "MvecScaleY.*=.*1\.0" src/dlss/dlss_fg.cpp → ≥1
      3. grep "pNoPostProcessingColor" src/dlss/dlss_fg.cpp → 0
      4. grep "pOutputRealFrame.*nullptr" src/dlss/dlss_fg.cpp → ≥1
      5. grep "ResourceNeverProvided" src/dlss/dlss_fg.cpp → ≥1
      6. grep -E "1\.0[fF]?\s*/\s*\(?(in\.render_w|render_w|width)" src/dlss/dlss_fg.cpp → 0
    Evidence: .omo/evidence/task-27-grep-guards.txt

  Scenario: SR 与 FG 共享 depth/MV(failure prevention)
    Tool: ast_grep
    Steps:
      1. 在主管线 T38 的 dispatch sequence 中,depth ImageView 必须用同一个 PooledTextureHandle
      2. ast_grep 验证 SR + FG eval 接同一 image handle
    Evidence: .omo/evidence/task-27-shared-depth-mv.txt
  ```

  **Commit**: YES
  - Message: `feat(dlss): FG Eval pass with multi-frame loop, ResourceNeverProvided, §-citations`
  - Files: `src/dlss/dlss_fg.h`、`dlss_fg.cpp`、`tests/test_dlss_fg.cpp`

- [x] 28. Stage 6 — Inverse Tonemap compute shader(对称 T26)

  **What to do**:
  - 写 `src/shaders/inverse_tonemap_eotf.comp`(GLSL compute,与 T26 对称),push constant `int mode`:
    - **`mode = 0` LDR path**(`is_hdr=false`):仅 sRGB EOTF⁻¹
      ```glsl
      // sRGB EOTF inverse: encoded [0,1] → scene-linear (clipped)
      vec3 linear = mix(encoded / 12.92,
                       pow((encoded + 0.055) / 1.055, vec3(2.4)),
                       step(0.04045, encoded));
      ```
    - **`mode = 1` HDR path**(`is_hdr=true`):PQ EOTF⁻¹ + ACES Filmic⁻¹
      ```glsl
      // Step 1: Inverse PQ ST.2084 EOTF (encoded [0,1] → tonemapped linear [0,1])
      const float m1 = 0.1593017578125;
      const float m2 = 78.84375;
      const float c1 = 0.8359375;
      const float c2 = 18.8515625;
      const float c3 = 18.6875;
      vec3 E = pow(encoded, vec3(1.0 / m2));
      vec3 num = max(E - c1, vec3(0.0));
      vec3 den = c2 - c3 * E;
      vec3 tonemapped = pow(num / den, vec3(1.0 / m1)) * 100.0;  // *100 反 T26 *0.01

      // Step 2: Inverse ACES Filmic (Narkowicz approximation, 数值反演)
      // 解 ACES 公式 y = (x*(a*x + b)) / (x*(c*x + d) + e) for x given y
      // 二次方程: (c*y - a) * x^2 + (d*y - b) * x + (e*y) = 0
      const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
      vec3 A = c * tonemapped - vec3(a);
      vec3 B = d * tonemapped - vec3(b);
      vec3 C = e * tonemapped;
      vec3 disc = B * B - 4.0 * A * C;
      vec3 scene_linear = (-B - sqrt(max(disc, 0.0))) / (2.0 * A);
      scene_linear = max(scene_linear, vec3(0.0));  // 防负值
      ```
    - 输出 scene-linear,供 EXR write 或 ProRes display-ready 写出前 fork
  - 写 `src/gpu/passes/inverse_tonemap_pass.h` + `.cpp`
  - **管线 wiring**:Stage 6 仅在 FG enabled 时调用(SR-only path 完全跳过);如果 Stage 7 输出 ProRes 422 HQ(display-ready),Stage 6 在 ProRes 写入前 fork — Stage 7 EXR 路径走 Stage 6,Stage 7 ProRes 路径直接用 Stage 5 输出
  - Catch2 测试:与 T26 round-trip 误差 < 0.02(ACES 数值反演容差略大);LDR sRGB round-trip 误差 < 0.01

  **Must NOT do**:
  - 不要复用 T26 shader(分开两个 shader 文件,清晰)
  - 不要在 SR-only path 调用(per Must Have)
  - 不要在 inverse 时再 tonemap
  - 不要忽略 ACES inverse 的负值情况(`max(scene_linear, 0)` 必须有,否则 disc < 0 时产生 NaN)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: 公式已知 + 二次方程数值反演,T26 对称
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4
  - **Blocks**: 37, 38
  - **Blocked By**: 5, 17, 26(共享 mode 常量 + ACES 系数)

  **References**:
  - 同 T26:SMPTE ST.2084、sRGB IEC、ACES Filmic Narkowicz approximation
  - ACES inverse 数值方法(二次公式)
  - **WHY**:FG 输出是 display-ready PQ encoded,EXR 写入需 scene-linear → 必须逆变换

  **Acceptance Criteria**:
  - [ ] inverse_tonemap shader 编译为 SPIR-V
  - [ ] LDR sRGB round-trip(linear → tonemap → inverse → linear)误差 < 0.01(linear < 1.0 的范围内)
  - [ ] HDR ACES+PQ round-trip 误差 < 0.02(linear < 16.0 范围内,极高亮度精度损失更大)
  - [ ] HDR inverse 不产生 NaN/Inf 像素

  **QA Scenarios**:
  ```
  Scenario: LDR sRGB round-trip(happy)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(0.5, 0.7, 0.9) scene-linear (in [0,1] range)
      2. tonemap_pass(LDR) → display-ready
      3. inverse_tonemap_pass(LDR) → scene-linear'
      4. assert |result - input| < 0.01
    Evidence: .omo/evidence/task-28-ldr-srgb-roundtrip.txt

  Scenario: HDR ACES+PQ round-trip(happy)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(0.18, 1.0, 4.0)(mid-grey, white, high luminance)
      2. tonemap_pass(HDR) → ACES + PQ encoded
      3. inverse_tonemap_pass(HDR) → inverse PQ + inverse ACES → scene-linear'
      4. assert |result - input| / max(input, 0.001) < 0.02 per channel
    Evidence: .omo/evidence/task-28-hdr-roundtrip.txt

  Scenario: HDR inverse 极端值不 NaN(failure prevention)
    Tool: Catch2 + GPU
    Steps:
      1. 输入 vec3(1.0, 1.0, 1.0)(PQ encoded 最大值)
      2. inverse_tonemap_pass(HDR)
      3. 验证输出无 NaN/Inf,数值有限
    Evidence: .omo/evidence/task-28-hdr-extreme-no-nan.txt
  ```

  **Commit**: YES
  - Message: `feat(gpu): inverse tonemap+EOTF (LDR sRGB / HDR PQ + ACES inverse)`
  - Files: `src/shaders/inverse_tonemap_eotf.comp`、`src/gpu/passes/inverse_tonemap_pass.h`、`.cpp`、`tests/test_inverse_tonemap.cpp`

- [x] 29. Stage 7 — Async writer + multi-format encoder(LDR PNG/EXR/ProRes;HDR EXR/ProRes)

  **What to do**:
  - 写 `src/io/async_writer.h` + `async_writer.cpp`:
    - `class AsyncFrameWriter`:
      - 一个 worker thread pool(默认 4 线程)
      - `enqueue(WriteJob{ format, path, data, width, height })`(SPMC queue)
      - WriteJob format enum(**最小支持** per 用户决议 round 2):
        - `ExrLinear` — `Imf::OutputFile`,R16G16B16A16_HALF,scene-linear
        - `Png16` — libpng / OpenEXR PNG,16-bit RGBA,**LDR 项目专用**(不开 HDR mode)
        - `ProRes422HQ` — FFmpeg `prores_ks` profile=3,容器 mov,10-bit,pixel format `yuv422p10le`
      - LDR 项目允许 EXR + PNG + ProRes;HDR 项目允许 EXR + ProRes(无 PNG,HDR PNG 边缘形式不主流)
      - 每个 frame 完成后 LOG_INFO 进度
    - **重编号**(per 用户 Q2 决议):FG 输出帧重编号 `0001..0(2N-1)`(image)或单文件累加(video);目录单独
    - **文件名模板**(per 用户 round 2 UX 决议):接受占位符:
      - `####` 或 `{frame:04d}` — 4-digit zero-padded frame number(默认)
      - `##` — 2-digit version suffix(用于 auto-version 模式)
      - 其它字面字符串保留
      - 默认模板:`frame_####.<ext>`(`.exr` / `.png` / 视频用文件名 `output.mov`)
    - **覆盖处理**(3 模式 enum,从 RPC `setTask` 传入):
      - `Skip` — 文件存在则跳过该帧 + LOG_WARN
      - `Overwrite` — 直接覆盖(默认)
      - `AutoVersion` — 检测 `<name>_v01.<ext>`,版本号自增到不冲突
    - **冲突批量预检**:Start 触发后,async writer 第一步预扫输出目录,统计冲突数,通过 RPC 通知 GUI(GUI 弹 confirmation dialog before actual write)
  - 在 T3 CMake 链 vcpkg ffmpeg(`prores_ks` 编码器 + libpng);v2 build flags 启用 prores_ks + 静态链接
  - **扩展口预留**(per 用户 round 2:留口子 + docs):
    - `enum class OutputFormat { ExrLinear, Png16, ProRes422HQ, /* not implemented in v2 - 7 future extension hooks: */ Jpeg, H264Mp4, H265Mp4, Av1Mp4, Avif, ProRes4444XQ, DNxHR }` — 后续 7 项 enum 占位,**不**注册到 EncoderRegistry
    - `EncoderRegistry` 接口设计支持插件式 codec 注册(v2 仅注册 3 种 implemented,扩展时加 `register("h264", ...)`)
    - `docs/cli_reference.md` 列出未来扩展计划
  - Catch2 测试:
    - 异步 enqueue 50 个 EXR job → 全部写入 OK + 验证 OpenEXR 读回数据一致
    - PNG 16-bit:30 帧 1080p RGBA → PNG 文件 ImageMagick `identify` 验证 16-bit + RGBA
    - ProRes encoding:30 帧 1080p → MOV 文件能用 ffprobe 验证 codec=prores + profile=3 + 10-bit
    - 文件名模板:`{frame:04d}` / `####` / `frame_####_v##` 三种解析正确
    - 覆盖模式:Skip / Overwrite / AutoVersion 各一条用例
    - 错误处理:磁盘满 / path 不可写 → 错误传播到 main thread

  **Must NOT do**:
  - 不要在主渲染线程 sync write(必须 async pool)
  - **不要支持** DNxHR / ProRes 4444 XQ / JPEG / H.264 / H.265 / AV1 / AVIF(均为扩展口,**v2 不实现**;`OutputFormat` enum 列出但不注册到 `EncoderRegistry`)
  - 不要硬编码线程数(可配 env `DLSS_COMPOSITOR_WRITER_THREADS`)
  - 不要在 ProRes 输出做 EOTF 反转(Stage 6 在 EXR 路径之前,ProRes 路径直接用 Stage 5 display-ready 输出)
  - 不要让 HDR 项目走 PNG(`if is_hdr && format == Png16` → reject with error)
  - 不要在 `AutoVersion` 模式撞到 ext 名(`frame_v##.exr.tmp` 不算冲突)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 多线程 + FFmpeg + libpng + OpenEXR + 文件名模板解析 + 3 种覆盖模式 + 扩展口设计
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4
  - **Blocks**: 36, 37, 38
  - **Blocked By**: 3(vcpkg ffmpeg + libpng)、5

  **References**:
  - OpenEXR `Imf::OutputFile` docs
  - FFmpeg libavcodec `prores_ks` encoder
  - libpng 16-bit RGBA encoding
  - 用户 round 2 决议:最小支持 + FFmpeg DLL 已有 + 留扩展口 + 完整文件名 UX
  - **WHY**:async writer 防止 GPU 等磁盘;LDR 用户多用 PNG;HDR 用户多用 EXR + ProRes;扩展口对齐 Blender 5.1 docs(PNG/JPEG/WebP/AVIF/TIFF/JPEG 2000/H.264/H.265/AV1)

  **Acceptance Criteria**:
  - [ ] async writer 50 job 全成功,主线程不 block(测量提交耗时 < 5 ms)
  - [ ] PNG 16-bit:`identify -format "%[channels] %[depth]" out.png` → "rgba 16"
  - [ ] ProRes MOV 用 ffprobe 验证 codec=prores + profile=3 + bits_per_raw_sample=10
  - [ ] grep 全 codebase **EncoderRegistry 注册行**无 `register\("dnxhd"|"prores_4444"|"jpeg"|"h264"|"h265"|"av1"|"avif"|"webp"\)`(扩展项仅 enum 占位 + docs 提及,**不**注册)— 详见"扩展 Codec 多维政策表"
  - [ ] 文件名模板 3 种解析正确(`####` / `{frame:04d}` / `frame_####_v##`)
  - [ ] 3 种覆盖模式各一条用例通过
  - [ ] HDR 项目尝试 PNG → reject with error code

  **QA Scenarios**:
  ```
  Scenario: 50 帧 async EXR(happy)
    Tool: Catch2
    Steps:
      1. AsyncFrameWriter w(4 threads)
      2. for i in 0..49: w.enqueue({ExrLinear, "frame_####.exr"⇒"frame_0001.exr"..., ...})
      3. w.flush() 等所有完成
      4. for i: 读回 EXR,与 data_i 像素 hash 一致
    Evidence: .omo/evidence/task-29-async-50.txt

  Scenario: PNG 16-bit LDR(happy)
    Tool: Bash + ImageMagick identify
    Steps:
      1. enqueue 30 个 1080p PNG job 到 frame_####.png
      2. flush
      3. identify -format "%[width] %[height] %[channels] %[depth]" frame_0001.png → "1920 1080 rgba 16"
    Evidence: .omo/evidence/task-29-png-16bit.txt

  Scenario: ProRes 422 HQ encode(happy)
    Tool: Bash + ffprobe
    Steps:
      1. enqueue 30 个 1080p ProRes422HQ job 到 video.mov
      2. flush
      3. ffprobe video.mov → codec=prores, profile=3 (422 HQ), bits_per_raw_sample=10
    Evidence: .omo/evidence/task-29-prores-ffprobe.txt

  Scenario: 文件名模板解析(happy)
    Tool: Catch2
    Steps:
      1. format_path("frame_####.exr", 42) → "frame_0042.exr"
      2. format_path("{frame:04d}.png", 1) → "0001.png"
      3. format_path("output_####_v##.mov", 100, version=2) → "output_0100_v02.mov"
    Evidence: .omo/evidence/task-29-template.txt

  Scenario: 覆盖模式 AutoVersion(happy edge)
    Tool: Catch2
    Steps:
      1. 创建 frame_0001.exr, frame_0001_v01.exr, frame_0001_v02.exr
      2. enqueue WriteJob(AutoVersion, "frame_####.exr", frame=1)
      3. 期望写入 frame_0001_v03.exr
    Evidence: .omo/evidence/task-29-autoversion.txt

  Scenario: HDR + PNG reject(failure path)
    Tool: Catch2
    Steps:
      1. setup is_hdr=true
      2. enqueue WriteJob(Png16, ...) → WriteError{HdrPngUnsupported}
      3. logger LOG_ERROR 含 "PNG 16-bit not supported for HDR project"
    Evidence: .omo/evidence/task-29-hdr-png-reject.txt

  Scenario: 不支持的扩展 codec(failure prevention)
    Tool: grep
    Steps:
      1. grep -rE "register.*\"h264\"|register.*\"h265\"|register.*\"dnxhd\"|register.*\"av1\"" src/io/async_writer.cpp → 0 匹配(扩展口仅在 enum 中,不注册)
    Evidence: .omo/evidence/task-29-no-extra-codecs-registered.txt

  Scenario: 磁盘满 graceful(failure)
    Tool: Catch2
    Steps:
      1. mock 写入 errno=ENOSPC
      2. enqueue → flush → 主线程拿到 WriteError{DiskFull}
      3. logger LOG_ERROR 含 "disk full"
    Evidence: .omo/evidence/task-29-disk-full.txt
  ```

  **Commit**: YES
  - Message: `feat(io): async writer (EXR/PNG/ProRes 422 HQ) with template + overwrite UX`
  - Files: `src/io/async_writer.h`、`async_writer.cpp`、`src/io/encoder_registry.h`、`tests/test_async_writer.cpp`、`CMakeLists.txt`(链 ffmpeg + libpng)

- [x] 30. Flutter native plugin bridge(MethodChannel + EventChannel + FFI Pointer + 共享内存)

  **What to do**:
  - 在 `gui/dlss_compositor_plugin/` 实现 plugin bridge:
    - **Native (C++)**:`gui/dlss_compositor_plugin/windows/dlss_compositor_plugin.cpp`
      - 链入 `dlss_engine_lib`(static lib from src/)
      - `class DlssCompositorPlugin : public flutter::Plugin`:
        - `RegisterWithRegistrar`:注册 MethodChannel `dlss_compositor/method` + EventChannel `dlss_compositor/events`
        - `HandleMethodCall(call, result)`:dispatch 到 `RpcDispatcher`(T31 提供)
        - `OnListen(arguments, events_sink)`:存 event_sink,后台 std::thread 渲染时 push event
      - **关键 threading 模式**(per `bg_36a24283`):
        - 渲染在 std::thread 跑
        - 渲染线程 push 事件:**先 capture event_sink + 用 `engine->ui_task_runner()->PostTask([sink, evt] { sink->Success(evt); })`** post 到 Flutter 主线程
        - 直接 std::thread 调 sink->Success() 会崩溃(参考研究报告 §B)
      - **预览帧 FFI 共享内存**:
        - C++ 端预分配 ring buffer(4 个 8MB 槽)
        - 渲染期间写入 ring,通过 EventChannel push `{frame_id, ptr_address, size}`
        - Dart 端通过 `dart:ffi` `Pointer.fromAddress(ptr_address).asTypedList(size)` 零拷贝读取
        - 读完后 EventChannel push `frame_consumed { frame_id }` 让 C++ 释放槽
      - 提供 helper `static void post_to_flutter_thread(std::function<void()> fn)`
    - **Dart**:`gui/lib/plugin/dlss_compositor.dart`
      - `DlssCompositor` class 暴露:
        - Future<void> setTask(Map params)
        - Future<bool> checkReady()
        - Future<DlssStatus> getStatus()
        - Future<TaskState> getTaskState()
        - Stream<Progress> get progressStream  // EventChannel
        - Stream<PreviewFrame> get previewStream  // EventChannel + FFI
        - Future<void> setDebugMode(String mode)  // experimental, may noop
      - `PreviewFrame { int width, height; Uint8List data; }`,内部用 `Pointer.fromAddress` 构造 Uint8List(零拷贝 view)
  - 写 plugin manifest `pubspec.yaml`:`plugin.platforms.windows.pluginClass = DlssCompositorPlugin`
  - Catch2 native side test(`tests/test_plugin_bridge.cpp`):mock flutter::MethodChannel,验证 method dispatch
  - Dart side test(`gui/dlss_compositor_plugin/test/method_channel_test.dart`):mock channel,验证 RPC roundtrip
  - **integration test**:启动 Flutter app + 调 setTask(只设虚拟 task,不真启动管线)→ 验证 method 到达 native side(用 LOG 痕迹)

  **Must NOT do**:
  - **不要在 std::thread 直接调 event_sink->Success()**(线程亲和性问题,会崩)
  - **不要用 EventChannel 传 8MB 二进制**(序列化开销;预览帧用 FFI 零拷贝)
  - 不要让 MethodChannel handler 阻塞 > 10ms(用 lock-free 状态读)
  - 不要在 plugin DLL 暴露 C++ 异常(必须 try/catch + 转换 Result)
  - 不要让 plugin 含 main 函数(纯 lib 模式)
  - 不要忘记 PDB 部署(Debug build 链 PDB,VSCode F5 用)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 跨 Dart/C++ 边界 + 共享内存生命周期 + 线程亲和性 + Flutter 平台 quirks
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO(主路径,Wave 5 第一项)
  - **Parallel Group**: Wave 5(然后 31-35 并行)
  - **Blocks**: 31, 32, 33, 34, 35
  - **Blocked By**: 8, 17(`dlss_engine_lib` 必须 link-able)

  **References**:
  - `bg_36a24283` 调研报告 §B(threading model)+ §C(binary data passing)+ §D(concurrency)
  - `flutter/engine` Windows embedder source(post_to_ui_thread pattern)
  - Flutter plugin_ffi docs
  - **WHY**:这是 v2 关键技术风险点,研究已 GO 但落地必须严格按 canonical pattern

  **Acceptance Criteria**:
  - [ ] `flutter build windows --release` 成功 + plugin DLL 出现在 build/windows/x64/runner/Release/
  - [ ] Dart 端 `setTask({...})` → native 端 LOG_INFO 收到 method call
  - [ ] EventChannel 一次 emit → Dart `progressStream.first` 收到
  - [ ] 共享内存 round-trip:C++ 写 8MB pattern → EventChannel 推 ptr → Dart 读取 + hash 一致(零拷贝验证可用 ptr 地址)
  - [ ] benchmark:8MB 预览帧端到端延迟 < 50ms(MethodChannel + FFI)
  - [ ] grep `event_sink->Success` outside `post_to_flutter_thread` → 0 匹配(线程安全)

  **QA Scenarios**:
  ```
  Scenario: setTask round-trip(happy)
    Tool: flutter integration test + log
    Steps:
      1. flutter integration test 启动 app
      2. await DlssCompositor.setTask({'config': '...'})
      3. native LOG 含 "setTask received"
      4. result == ok
    Evidence: .omo/evidence/task-30-settask-roundtrip.txt

  Scenario: 8MB 预览帧零拷贝(critical happy)
    Tool: flutter integration test
    Steps:
      1. native 在 ring buffer slot 0 写 8MB pattern (0xAB 0xCD ...)
      2. EventChannel emit { frame_id: 1, ptr: addr_0, size: 8MB }
      3. Dart Pointer.fromAddress(addr_0).asTypedList(8MB)
      4. 验证前 1024 字节 == pattern
      5. 测量耗时 < 50ms(从 native 写完到 Dart 验证完)
    Evidence: .omo/evidence/task-30-zero-copy.txt

  Scenario: 后台线程 push 不崩(failure prevention)
    Tool: stress test
    Steps:
      1. 启 1 个 std::thread,500ms 间隔 push event 100 次
      2. 验证 Dart 端 receive 100 次,无崩溃
      3. ASAN/TSAN 验证无 race condition
    Evidence: .omo/evidence/task-30-thread-push-100.txt

  Scenario: 直接 std::thread 调 sink 检测(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'event_sink->Success($$$)' gui/dlss_compositor_plugin/windows/
      2. 每个匹配点必须在 lambda 内,且 lambda 通过 post_to_flutter_thread 调用
      3. 无 inline 直接调用
    Evidence: .omo/evidence/task-30-thread-affinity-grep.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): native plugin bridge with MethodChannel + EventChannel + FFI shared memory`
  - Files: `gui/dlss_compositor_plugin/windows/*.cpp`、`*.h`、`gui/lib/plugin/dlss_compositor.dart`、tests, CMake

- [x] 31. dlss/* RPC 接口实现(双 transport 共用 RpcDispatcher)

  **What to do**:
  - 写 `src/rpc/dispatcher.h` + `dispatcher.cpp`:
    - `class RpcDispatcher`:transport-agnostic,接 `nlohmann::json` 请求,返回 `nlohmann::json` 响应 + 异步 push events
    - 注册 handlers:
      - `setTask(params)` → 验证 dlssconfig.json 有效 + memorize task config + 返回 `{ok, task_id}`
      - `checkReady()` → 检查 NGX SR/FG 可用 + GPU 显存足够 + 输入 EXR 序列存在 + 返回 `{ready: bool, reason?}`
      - **`checkOutputCollisions(template, dir, format)`** → 预扫输出目录,返回 `{collisions: int, files: [path...]}`(per T33 confirmation dialog 流程,Start 前调用)
      - `startTask(task_id)` → 触发管线启动(异步,在 worker thread 跑)
      - `getStatus()` → 返回 `{dlss_version, fg_max_multiplier, gpu_name, driver_version, is_hdr_supported, video_encoders: ["prores_ks"], export_formats_hdr: ["exr", "prores_422hq"], export_formats_ldr: ["png_16", "exr", "prores_422hq"]}`(per 用户 round 2:LDR 加 PNG,HDR 不加 PNG)
      - `getTaskState(task_id)` → `{state, current_frame, total_frames, eta_seconds}`
      - `getProgress()` → lock-free 读 atomics,< 10ms
      - `getPreviewFrame(frame_index)` → 返回最新预览帧(走 FFI 共享内存通道,response 仅含 ptr+size)
      - `setDebugMode(mode)` → 标 experimental,初始返回 `{ok: false, reason: "not implemented"}`
    - **lock-free progress struct**(per Metis):
      ```cpp
      struct alignas(64) RenderProgress {
          std::atomic<int> current_frame{0};
          std::atomic<int> total_frames{0};
          std::atomic<float> percent{0.0f};
          std::atomic<int> state{Idle};  // enum: Idle, Running, Done, Error
      };
      ```
    - **Command queue**(SPSC):MethodChannel handler 推 cmd,渲染 worker 拉
  - 写 `src/rpc/transport_native.cpp`:对接 T30 plugin,把 dispatcher 接到 MethodChannel handler
  - 写 `src/rpc/transport_stdio.cpp`:JSON-RPC over stdio with Content-Length framing(per `bg_03036ae9` 报告 ~300 LOC),用于 fallback CLI exe
  - 写 `src/main_cli.cpp`:JSON-RPC stdio 入口,链 dispatcher + transport_stdio,产出 `dlss-compositor-cli.exe`
  - Catch2 单测:每个 method 一条 test,mock dispatcher inputs → 验证 outputs

  **Must NOT do**:
  - 不要在 transport 里跑 dispatcher 业务逻辑(职责分离)
  - 不要在 getProgress / getTaskState handler 加锁(lock-free 原子读)
  - 不要让 setTask 同步等管线完成(必须 enqueue + 立即返回 task_id)
  - 不要在 RpcDispatcher 里写日志到 stdout(用 spdlog → stderr)
  - 不要在 response 里塞 8MB binary(预览帧走 FFI 共享内存,RPC response 仅 metadata)
  - **不要在 video_encoders / export_formats 列** dnxhd / prores_4444 / jpeg / h264 / h265 / av1 / avif / webp(扩展项,v2 不实现);**LDR 项目允许 png_16**;HDR 项目不允许 png_16(per T29 reject)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 接口工程量大 + 跨 transport + 异步状态机
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES(after T30)
  - **Parallel Group**: Wave 5
  - **Blocks**: 36, 37, 38(管线 wiring 用 dispatcher)
  - **Blocked By**: 30, 18, 19, 20, 21, 22, 23, 24, 27, 29

  **References**:
  - `bg_03036ae9` JSON-RPC stdio C++ 报告
  - `bg_36a24283` Flutter native plugin 报告(D 节 lock-free progress)
  - **WHY**:这是 GUI ↔ 引擎的实际接口层;双 transport 共用 dispatcher 让回退 JSON-RPC 时无业务改动

  **Acceptance Criteria**:
  - [ ] 9 个 RPC method 各一个单测全过(setTask / checkReady / **checkOutputCollisions** / startTask / getStatus / getTaskState / getProgress / **getPreviewFrame** / setDebugMode)
  - [ ] getStatus 返回的 video_encoders 仅含 `["prores_ks"]`,export_formats_ldr 含 `["png_16", "exr", "prores_422hq"]`,export_formats_hdr 含 `["exr", "prores_422hq"]`(per T29 + 用户 round 2)
  - [ ] checkOutputCollisions 实现 + 返回结构正确
  - [ ] grep RPC handler **video_encoders / export_formats_* 返回值列表**无 `dnxhd|prores_4444|jpeg|h264|h265|av1|avif|webp`(扩展项 enum 允许;返回值不允许 — 详见"扩展 Codec 多维政策表")
  - [ ] startTask 异步:同步耗时 < 100ms,实际渲染在 worker
  - [ ] dlss-compositor-cli.exe build 成功 + `--rpc-stdio` 模式启动 + 接受 JSON-RPC 请求

  **QA Scenarios**:
  ```
  Scenario: 9 method round-trip(happy)
    Tool: Catch2
    Steps:
      1. mock dispatcher backend
      2. for each of 9 methods: invoke + assert response shape
    Evidence: .omo/evidence/task-31-9-methods.txt

  Scenario: stdio JSON-RPC fallback(happy)
    Tool: Bash + 模拟 stdin
    Steps:
      1. echo '{"jsonrpc":"2.0","method":"getStatus","id":1}' | ./dlss-compositor-cli.exe --rpc-stdio
      2. 收到正确 JSON 响应,Content-Length framed
      3. 验证 video_encoders == ["prores_ks"]
    Evidence: .omo/evidence/task-31-stdio-getstatus.txt

  Scenario: 未实现 codec 不在 EncoderRegistry 注册(failure prevention)
    Tool: grep
    Steps:
      1. grep -ri "register.*\"h264\"\|register.*\"h265\"\|register.*\"av1\"\|register.*\"dnxhd\"\|register.*\"dnxhr\"\|register.*\"prores_4444\"\|register.*\"webp\"\|register.*\"jpeg\"\|register.*\"avif\"" src/rpc/ src/io/ → 0(扩展项不注册)
      2. 仅 prores_ks 注册(`getStatus().video_encoders == ["prores_ks"]`)
    Evidence: .omo/evidence/task-31-no-extra-codec-registered.txt

  Scenario: getProgress lock-free 性能(happy)
    Tool: Catch2 benchmark
    Steps:
      1. 渲染 worker 跑(模拟,10ms 间隔更新 progress)
      2. dispatcher.getProgress() × 10000 次,平均 < 1us
    Evidence: .omo/evidence/task-31-progress-perf.txt
  ```

  **Commit**: YES
  - Message: `feat(rpc): RpcDispatcher with MethodChannel + JSON-RPC stdio dual transport`
  - Files: `src/rpc/dispatcher.h`、`dispatcher.cpp`、`transport_native.cpp`、`transport_stdio.cpp`、`src/main_cli.cpp`、`tests/test_rpc_dispatcher.cpp`、`tests/test_rpc_stdio.cpp`

- [ ] 32. Flutter 状态栏 + dlssconfig 选择器

  **What to do**:
  - 写 `gui/lib/widgets/status_bar.dart`:
    - 底部固定状态栏(类似 VS Code)
    - **左侧**:DLSS SDK 版本 + GPU 名称 + FG max multiplier(从 `getStatus` RPC 拉,启动时一次)
    - **右侧**:任务进行时显示进度条 `[████░░░░] 42/120`(订阅 `progressStream`,500ms throttle)
    - 静默时右侧空白
  - 写 `gui/lib/widgets/dlssconfig_picker.dart`:
    - 顶部按钮 "Open dlssconfig.json"
    - 点击调用 T8 的 `selectFile()`(Win32 FileOpenDialog,仅 .json filter)
    - 选中后:解析 JSON + 用 schema validate + 显示路径 + 状态(✅ 有效 / ❌ 错误 + reason)
    - **schema validation**:Dart 用 `package:json_schema` 库读 `src/core/dlssconfig.schema.json`(打包时复制到 assets)— 这是 single source of truth 的 Dart 端使用
    - 解析后从 dlssconfig 提取并显示:`output_resolution`、`render_resolution`、`quality_mode`、`is_hdr`、`fps`、`frames.length`
  - 写 `gui/lib/state/app_state.dart`:用 `provider` package,持有 `currentDlssConfig`、`taskState`、`status` 等全局状态
  - 写 `gui/lib/main.dart`:`MaterialApp` + `Scaffold`(body 是 task config 面板 + detail 面板的占位 + statusBar bottomBar)
  - widget tests:
    - status_bar:mock RPC 返回 → 验证版本字符串显示
    - dlssconfig_picker:mock file dialog 选 valid json → 显示 ✅;选 invalid json → 显示 ❌ + reason

  **Must NOT do**:
  - 不要在 GUI 端复制 schema 文件(从 src/core/ 引用作为 Flutter assets)
  - 不要在 picker 显示完整 JSON(只 summary,避免界面爆炸)
  - 不要让 status bar 阻塞主线程(progress stream throttle + async)
  - 不要硬编码 GPU 名称(从 RPC 拿)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Flutter UI + 状态管理 + 视觉布局
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5
  - **Blocks**: 33, 34(detail / config 面板基于 selected config)
  - **Blocked By**: 8, 30, 31

  **References**:
  - `package:provider` docs
  - `package:json_schema` Dart lib
  - `src/core/dlssconfig.schema.json`(single source of truth)
  - **WHY**:用户 entry point 是这两个 widget;dlssconfig 选错会导致后续所有面板信息错乱

  **Acceptance Criteria**:
  - [ ] status bar 显示 DLSS 版本 + GPU + FG max
  - [ ] picker 验证有效 dlssconfig.json → 显示 summary 信息
  - [ ] picker 拒绝 invalid json → 显示 schema error 信息(如 `jitter_x out of range at frames.0042`)
  - [ ] schema 文件作为 Flutter assets 一致性 verified(MD5 对比 src 端)

  **QA Scenarios**:
  ```
  Scenario: 状态栏显示(happy)
    Tool: flutter integration test
    Steps:
      1. 启动 app
      2. mock getStatus 返回 {dlss_version: "310.6.0", gpu: "RTX 4090", fg_max: 4}
      3. 验证 status bar 含 "DLSS 310.6.0 | RTX 4090 | FG Max 4x"
    Evidence: .omo/evidence/task-32-statusbar.png

  Scenario: 选择 valid dlssconfig(happy)
    Tool: integration test
    Steps:
      1. mock file dialog 返回 fixtures/sample-dlssconfig.json
      2. 点击 "Open" 按钮
      3. UI 显示 ✅ + summary(3840×2160, Balanced, 30 fps, 60 frames, HDR)
    Evidence: .omo/evidence/task-32-pick-valid.png

  Scenario: invalid dlssconfig 错误显示(failure)
    Tool: integration test
    Steps:
      1. mock dialog 返回 fixtures/dlssconfig-bad-jitter.json
      2. UI 显示 ❌ + "jitter_x = 0.7 out of range at frames.0042"
    Evidence: .omo/evidence/task-32-pick-bad.png

  Scenario: Schema 一致(failure prevention)
    Tool: Bash
    Steps:
      1. md5sum src/core/dlssconfig.schema.json
      2. md5sum gui/assets/dlssconfig.schema.json (or 引用机制)
      3. 一致(或文件是 symlink/CMake copied)
    Evidence: .omo/evidence/task-32-schema-md5-match.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): status bar + dlssconfig picker with schema validation`
  - Files: `gui/lib/widgets/status_bar.dart`、`dlssconfig_picker.dart`、`gui/lib/state/app_state.dart`、`gui/lib/main.dart`、`gui/test/*`

- [ ] 33. Flutter 任务配置面板(扩展 output formats + filename template + overwrite UX)

  **What to do**:
  - 写 `gui/lib/widgets/task_config_panel.dart`:左侧容器(可拖拽分隔栏),含:
    1. **dlssconfig 选择器**(嵌入 T32)
    2. **加载后 readonly 信息**:output_resolution / render_resolution / quality_mode / is_hdr / fps / frames count / display_device / view_transform
    3. **Preset 选择器**(下拉框):Auto / J / K / L / M(默认 Auto,实际生效是按 quality 自动映射 K/L/M)
    4. **FG 选项**:
       - Toggle:启用 / 禁用 FG
       - 倍率 toggle button group `[2x] [3x] [4x]`,默认 2x;**最大倍率从 RPC `getStatus().fg_max_multiplier` 拉,超过的灰显**
    5. **输出配置 Section**(per 用户 round 2 完整 UX):
       - **输出目录**:文件夹选择器(Win32 FolderBrowserDialog FFI from T8)
         - 默认从 dlssconfig 推导(`<dlssconfig.dir>/output/`)
       - **文件名模板**:文本输入框 + 占位符提示
         - 默认 `frame_####.<ext>`(image)或 `output.mov`(video)
         - Hint:`####` = frame number 4 位 zero-pad,`{frame:04d}` 同义,`##` = version
         - 实时预览:输入下方显示 "Sample: frame_0001.exr" 等
       - **输出格式 dropdown**(per `dlssconfig.is_hdr` 动态过滤):
         - LDR 项目(`is_hdr=false`):`PNG (16-bit)` / `EXR (scene-linear)` / `ProRes 422 HQ` 三选(默认 PNG)
         - HDR 项目(`is_hdr=true`):`EXR (scene-linear)` / `ProRes 422 HQ` 二选(默认 EXR)
         - **不显示** DNxHR / ProRes 4444 / JPEG / H.264 / H.265 / AV1 / AVIF(扩展口预留,docs 提及未来支持)
       - **覆盖模式 radio**(3 选 1,per 用户 round 2):
         - `Skip`:文件存在跳过 + LOG_WARN
         - `Overwrite`:直接覆盖(默认)
         - `Auto-version (_v##)`:版本号自增到无冲突
    6. **Pre-flight 冲突检测**:Start 点击前 → RPC `dlss/checkOutputCollisions(template, dir, format)` → 返回冲突文件数;若 > 0 弹 confirmation dialog "Output dir 含 N 个潜在冲突文件,模式 = <selected>,继续?"
    7. **Start 按钮**:点击 → 通过 confirmation → 调 `setTask` RPC(传 output_dir + template + format + overwrite_mode)+ `startTask` → 状态栏开始更新进度
  - 用 `provider` 监听 `currentDlssConfig` 变化,自动刷新 readonly + 输出格式 dropdown 选项
  - widget tests + integration test(mock RPC):点击 Start → 验证 RPC 调用参数完整 + confirmation dialog 触发条件 + 各覆盖模式

  **Must NOT do**:
  - **不要在格式下拉框列** DNxHR / ProRes 4444 / JPEG / H.264 / H.265 / AV1 / AVIF / WebP(扩展口仅在 enum 与 docs,不进 GUI)
  - 不要让 user 手动改 render_resolution(readonly 显示)
  - 不要在 quality / FG 倍率超出硬件能力时仍允许选(`fg_max=2` 的 GPU 把 3x/4x 灰)
  - 不要在 LDR 项目下让 PQ encoding 选项可见
  - 不要在 HDR 项目下让 PNG 选项可见(per T29 reject)
  - 不要让模板字符串接受路径分隔符(`/` `\`)— 必须是仅文件名部分
  - 不要在 Skip 模式下不弹冲突 confirmation(用户可能不知道有跳过)— Skip 时也要列 N 帧将跳过

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Flutter UI 布局 + 状态绑定 + 条件 disable + 模板预览实时更新 + 冲突 dialog
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5
  - **Blocks**: 无(34 detail 面板独立)
  - **Blocked By**: 8, 30, 31, 32

  **References**:
  - `package:provider` ChangeNotifier
  - 用户 round 2 决议:LDR PNG/EXR/ProRes;HDR EXR/ProRes;最小支持 + 留扩展口
  - 用户 round 2 决议:文件名模板 + 覆盖 radio + confirmation dialog
  - **WHY**:这是任务发起入口,UI 选项必须严格匹配后端能力 + 文件路径冲突给用户清晰反馈

  **Acceptance Criteria**:
  - [ ] 配置面板含 7 个交互控件(preset / FG toggle / FG 倍率 / 输出目录 / 文件名模板 / 输出格式 / 覆盖模式 / start)
  - [ ] FG 倍率超过 fg_max 时灰显且不可选
  - [ ] 输出格式 dropdown 按 is_hdr 动态过滤(LDR 3 项,HDR 2 项)
  - [ ] **dropdown items 数组**(via ast_grep 检查)严格不含 DNxHR / 4444 / H.264 / H.265 / AV1 / AVIF / WebP / JPEG;dart 文档字符串 / 注释 / "Future Extensions" 提示文本可提及
  - [ ] 文件名模板实时预览:输入 `frame_####.exr` → 显示 "Sample: frame_0001.exr"
  - [ ] Start 前若 RPC 报冲突 > 0 → 弹 confirmation dialog
  - [ ] Skip / Overwrite / Auto-version 3 种模式下 RPC 参数正确传递

  **QA Scenarios**:
  ```
  Scenario: LDR 项目 PNG default + 完整流程(happy)
    Tool: integration test
    Steps:
      1. 加载 sample-dlssconfig-ldr.json (is_hdr=false)
      2. 验证 dropdown 选项 = ["PNG (16-bit)", "EXR (scene-linear)", "ProRes 422 HQ"],默认 PNG
      3. 选 preset = Auto, FG enabled 2x, format = PNG, dir = "C:\\out", template = "frame_####.png"
      4. mock checkOutputCollisions 返回 0 → 无 dialog,直接 startTask
      5. 验证 setTask 参数完整(format=Png16,template,overwrite_mode=Overwrite)
    Evidence: .omo/evidence/task-33-ldr-png-flow.txt

  Scenario: HDR 项目 EXR default(happy)
    Tool: integration test
    Steps:
      1. 加载 sample-dlssconfig-hdr.json (is_hdr=true)
      2. 验证 dropdown 选项 = ["EXR (scene-linear)", "ProRes 422 HQ"](2 项),默认 EXR
      3. 验证 PNG 选项不出现
    Evidence: .omo/evidence/task-33-hdr-exr-default.txt

  Scenario: 冲突检测 + dialog(happy edge)
    Tool: integration test
    Steps:
      1. 选输出目录 + 模板
      2. mock checkOutputCollisions 返回 5
      3. 点 Start → confirmation dialog 弹出 "5 个文件将被覆盖, 继续?"
      4. 用户点 Continue → setTask + startTask
    Evidence: .omo/evidence/task-33-conflict-dialog.txt

  Scenario: Auto-version 模式(happy)
    Tool: integration test
    Steps:
      1. 选 overwrite_mode = AutoVersion
      2. 模板 frame_####.exr
      3. mock 已存在 frame_0001.exr → AutoVersion 不报冲突(各模式预期可写入 v01)
      4. setTask 传 overwrite_mode=AutoVersion
    Evidence: .omo/evidence/task-33-autoversion.txt

  Scenario: 不显示扩展未实现编码器(failure prevention)
    Tool: ast_grep + integration test
    Steps:
      1. ast_grep --lang dart 'DropdownMenuItem<$T>(value: $V, child: Text("$LABEL"))' gui/lib/widgets/task_config_panel.dart → 收集所有 dropdown items LABEL
      2. LDR 项目 dropdown LABEL 集合应严格 == {"PNG (16-bit)", "EXR (scene-linear)", "ProRes 422 HQ"};不含 H.264/H.265/AV1/AVIF/WebP/JPEG/DNxHR/4444
      3. HDR 项目 LABEL 集合应严格 == {"EXR (scene-linear)", "ProRes 422 HQ"}
      4. 文档字符串 / 注释 / Future Roadmap 等 dart 代码区域可提及扩展项(不在此 grep 范围)
    Evidence: .omo/evidence/task-33-no-extra-formats.txt + screenshot
  ```

  **Commit**: YES
  - Message: `feat(gui): task config panel (preset/fg/output formats/filename template/overwrite UX)`
  - Files: `gui/lib/widgets/task_config_panel.dart`、`gui/test/task_config_test.dart`、`gui/integration_test/config_flow_test.dart`

  **QA Scenarios**:
  ```
  Scenario: Quality + FG 配置(happy)
    Tool: integration test
    Steps:
      1. 加载 sample-dlssconfig.json
      2. 选 preset = K
      3. 启 FG,选 2x
      4. 选输出格式 = ProRes 422 HQ
      5. 选输出目录
      6. 点击 Start
      7. 验证 setTask 调用参数完整,startTask 跟随
    Evidence: .omo/evidence/task-33-config-flow.txt

  Scenario: 不允许超 fg_max(failure prevention)
    Tool: integration test
    Steps:
      1. mock getStatus().fg_max_multiplier = 2
      2. 加载 dlssconfig
      3. 启 FG → 3x / 4x button 应 disabled (灰)
    Evidence: .omo/evidence/task-33-fg-max-disabled.png

  Scenario: dropdown 不显示扩展未实现编码器(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang dart 'DropdownMenuItem<$T>(value: $V, child: Text("$LABEL"))' gui/lib/widgets/task_config_panel.dart → 列出 LABEL
      2. LABEL 集合不含 "DNxHR" / "ProRes 4444" / "JPEG" / "H.264" / "H.265" / "AV1" / "AVIF" / "WebP";仅 dropdown items 在此约束,docs/注释允许提及
      2. 输出格式 dropdown 截图,人工验证只 2 项
    Evidence: .omo/evidence/task-33-format-dropdown.png + grep.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): task configuration panel with quality/preset/FG/output options`
  - Files: `gui/lib/widgets/task_config_panel.dart`、`gui/test/task_config_test.dart`

- [ ] 34. Flutter 详情面板(实时预览帧 + 进度 + 日志 + 完成统计)

  **What to do**:
  - 写 `gui/lib/widgets/detail_panel.dart`:右侧容器(可拖拽分隔栏)
    - **实时预览帧**:
      - 订阅 `previewStream` from T30(EventChannel + FFI 零拷贝)
      - 显示最新帧(`Image.memory(Uint8List.fromList(...))` 或 `dart:ui Image.fromRgbaBytes`)
      - 2-4 fps 更新(throttle 在 native 端)
      - **不**显示 DLSS debug overlay toolbar(per Metis Q4d:headless 不支持)
    - **进度信息**:
      - 当前帧 / 总帧数
      - ETA(从 `getTaskState` 拉)
      - state(Idle / Running / Done / Error)
    - **日志 panel**:
      - 订阅 `logStream`(T30 加 channel `dlss_compositor/log`)
      - 滚动列表显示最近 N=200 条 log
      - 按 level 着色(error 红,warn 黄,info 默认,debug 灰)
    - **完成统计**(state == Done 触发):
      - 总耗时、平均 fps、输入帧数、输出帧数(2x / 4x)、输出格式、输出目录
      - "Open Output Folder" 按钮(用 `Process.start('explorer.exe', [dir])`)
  - integration test:模拟 5 帧渲染流程,验证 4 个 sub-widget 都更新

  **Must NOT do**:
  - **不要实现 debug overlay toolbar**(headless 不支持,标 v2 不做)
  - 不要每帧 `Image.memory` 创建新 Image(用 `Image` widget + `key` reuse 减少重建)
  - 不要把 8MB 预览帧塞进状态(用 ValueNotifier + dispose old image)
  - 不要让日志 list 无限增长(rolling N=200)

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
    - Reason: Flutter UI + 流订阅 + 性能(高频图像更新)
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5
  - **Blocks**: 无
  - **Blocked By**: 8, 30, 31

  **References**:
  - Flutter `Image.memory` + dart:ui RawImage docs
  - `package:rxdart` for throttling
  - **WHY**:这是用户唯一的反馈面,慢/卡都直接破坏体验

  **Acceptance Criteria**:
  - [ ] 详情面板显示 4 个区(预览 / 进度 / 日志 / 完成统计)
  - [ ] 预览帧 2-4 fps 流畅(integration test 测量帧率)
  - [ ] 日志 rolling 200 条,新进旧出
  - [ ] state==Done 触发完成统计可见 + Open Folder 按钮工作
  - [ ] grep 全 dart 代码无 "debug_overlay" 或 setDebugMode 实际调用(只标 experimental)

  **QA Scenarios**:
  ```
  Scenario: 5 帧预览流畅(happy)
    Tool: integration test
    Steps:
      1. mock previewStream emit 5 个 8MB 帧 (250ms 间隔)
      2. UI 应在 1.5s 内全部 render
      3. 测量 frame render 平均 < 50ms
    Evidence: .omo/evidence/task-34-preview-rate.txt

  Scenario: 完成统计(happy)
    Tool: integration test
    Steps:
      1. mock state Idle → Running → Done
      2. 验证 Done 时统计 panel 显示
      3. 点击 Open Folder → mock Process.start 调用
    Evidence: .omo/evidence/task-34-done-stats.png

  Scenario: 日志 rolling(happy edge)
    Tool: integration test
    Steps:
      1. emit 250 条 log
      2. 验证 UI 仅显示最新 200 条
    Evidence: .omo/evidence/task-34-log-rolling.txt

  Scenario: 不实现 debug overlay(failure prevention)
    Tool: grep
    Steps:
      1. grep -ri "debug.*overlay\|setDebugMode" gui/lib/ → 仅出现在文档字符串/comments 中,无实际 button binding
    Evidence: .omo/evidence/task-34-no-debug-overlay.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): detail panel with live preview + progress + log + completion stats`
  - Files: `gui/lib/widgets/detail_panel.dart`、`gui/test/detail_panel_test.dart`

- [ ] 35. SharedPreferences 持久化 + debug/release 配置分离

  **What to do**:
  - 写 `gui/lib/state/preferences.dart`:
    - 用 `package:shared_preferences` 持久化:
      - `last_dlssconfig_path`(string)
      - `preset_override`(string,默认 "auto")
      - `fg_enabled`(bool)
      - `fg_multiplier`(int)
      - `output_format`(string)
      - `output_directory`(string)
      - `splitter_position`(double,左右分隔栏位置)
      - `session_title`(string,默认 `<aov_dir_name>_<random4>`,用户可自定义)
    - 启动时加载;UI 改动时 debounced(500ms)写回
  - **debug/release 分离**:
    ```dart
    final prefsKey = kDebugMode ? 'dlss_compositor_debug' : 'dlss_compositor_release';
    ```
    - debug build 用 separate prefs key,避免开发期污染用户配置
  - 加 `Reset to Defaults` 按钮(在右侧详情底部 / About 菜单)
  - widget test:
    - 模拟 set + restart → 配置恢复
    - debug 模式 prefs key 不与 release 重叠

  **Must NOT do**:
  - 不要把 dlssconfig.json 内容存到 prefs(只存路径)
  - 不要在 prefs 存 dial(Window position / 主题等基础 UI 状态留给 Flutter 自带 mechanism)
  - 不要在每次 UI 变化都立即写盘(debounced 500ms)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: SharedPreferences 标准 API,模板化
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5
  - **Blocks**: 无
  - **Blocked By**: 8, 33

  **References**:
  - `package:shared_preferences` docs
  - 用户决议:debug/release 分离配置入口
  - **WHY**:重启 app 不丢配置 + dev 实验不污染 release

  **Acceptance Criteria**:
  - [ ] 8 项配置全部持久化 + 启动恢复
  - [ ] kDebugMode 时使用 `dlss_compositor_debug` prefs key,Release 用 `_release`
  - [ ] Reset to Defaults 按钮清空 prefs 并恢复初始值
  - [ ] widget test 模拟 close+open 验证恢复

  **QA Scenarios**:
  ```
  Scenario: 8 项配置 round-trip(happy)
    Tool: widget test + flutter test
    Steps:
      1. set last_dlssconfig_path = "C:\\test.json"
      2. set fg_enabled = true, fg_multiplier = 4
      3. dispose + recreate prefs
      4. 验证全 8 项 == 之前 set
    Evidence: .omo/evidence/task-35-prefs-roundtrip.txt

  Scenario: Debug/release 分离(failure prevention)
    Tool: widget test
    Steps:
      1. release mode 写入 fg_multiplier = 2
      2. 切 debug mode(测试时 mock kDebugMode=true)
      3. 读 fg_multiplier → 应是 default 2x(空 prefs),不是 release 写入的值
    Evidence: .omo/evidence/task-35-debug-isolation.txt

  Scenario: Reset Defaults(happy)
    Tool: integration test
    Steps:
      1. 设几个 prefs
      2. 点 Reset Defaults
      3. 全部回到初始默认值
    Evidence: .omo/evidence/task-35-reset.txt
  ```

  **Commit**: YES
  - Message: `feat(gui): SharedPreferences with debug/release isolation`
  - Files: `gui/lib/state/preferences.dart`、`gui/test/preferences_test.dart`

- [ ] 36. SR-only 主流程拼装(Stage 1 → 2 → 7)

  **What to do**:
  - 写 `src/pipeline/sr_only_pipeline.h` + `sr_only_pipeline.cpp`:
    - `class SrOnlyPipeline`:接 dlssconfig + dispatcher,跑端到端 SR-only 流程
    - 每帧:
      1. **Stage 1**:`FramePrefetcher` 拉下一帧 → `ChannelMapper` 打包 → `TexturePool::acquire` SceneColor/Depth/MV/MV_DLSS 4 个 image → batch upload → `DepthConversionPass` + `MvPass` 跑两个 compute
      2. **Stage 2**:`DlssSrPass.evaluate` SR(scene-linear → scene-linear,output res)
      3. **Stage 7**:`AsyncFrameWriter.enqueue({ExrLinear or Png16, output_path})`(LDR 项目可走 PNG;HDR 项目走 EXR — 见 T29 format matrix)
      4. **Stage 7**:`AsyncFrameWriter.enqueue({Exr, output_path})`
      5. release texture handles,推预览帧到 EventChannel(订阅则发,无则跳)
      6. 更新 lock-free progress
    - **InReset 触发**:首帧 `frame_idx == 0` || `prev_frame.has_gap_after` || `cameraDiscontinuity(matrix_world_prev, matrix_world_curr) > threshold`
    - **cameraDiscontinuity 阈值**:**T36 不固定具体数值**(无文档/用户决议支持具体数字);改为 dlssconfig.json 提供可选 `camera_cut_thresholds: { translation, rotation_deg }` 字段(默认值未确定),**T38 端到端集成测试用真实场景动画 measurement 标定**,标定结果回填到默认值并 commit;期间 T36 用 sentinel 值(如 `INFINITY`,等价于"永不触发 cut except via gap")保证 first commit 不引入未验证常量
    - 后续可用 MV pass 平均位移异常作为兜底(per 用户 Q3:管线插一个 pass 读 MV)
  - 写 Catch2 集成测试(`[gpu]`):用 fixture 5 帧 → 5 输出 EXR,scene-linear,无 NaN
  - **关键 invariant**:depth/MV image 在 dispatcher 主循环复用同一份 `PooledTextureHandle`(per FG §6 L545,SR+FG 共享前提的延伸,SR-only 路径也保持同样 acquire/release 模式以便 T37/T38 复用)

  **Must NOT do**:
  - 不要在 SR-only 路径调 Stage 4/5/6(那是 FG 路径)
  - 不要每帧 vkCreateImage(全部 TexturePool acquire/release)
  - **不要忘记 Stage 1 的 depth/MV shader**(直接传 raw EXR data 给 SR 会让 SR 退化)
  - 不要在 reset 触发条件里少了 `has_gap_after`
  - **不要写 stdout**(进度走 dispatcher RPC)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 端到端编排,任何一处错位都会让 SR 失效;reset 逻辑是 v1 长期 bug 源
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO(串行,依赖 T37 复用)
  - **Parallel Group**: Wave 6 sequential
  - **Blocks**: 38
  - **Blocked By**: 6, 18, 19, 20, 21, 22, 23, 24, 29, 31(T25 已删,不再依赖)

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.1(SR 在 tonemap 之前 → SR-only path 全 scene-linear,无 tonemap)
  - SR §3.13(reset triggers)
  - **WHY**:第一个完整管线;若此处错,所有后续 task 的 GPU 测试都不准

  **Acceptance Criteria**:
  - [ ] SrOnlyPipeline 跑 5 帧 fixture → 5 个输出 EXR
  - [ ] 输出 EXR 是 scene-linear(R16G16B16A16_HALF,IEC channel = scene_linear via OpenEXR header `colorSpace`)
  - [ ] 帧 1 InReset=1,帧 2-5 InReset=0(用 mock SR 验证)
  - [ ] 模拟帧 gap(给 prefetcher 路径列表 [0001, 0003, 0004]):帧 0003 InReset=1
  - [ ] 模拟相机切(matrix discontinuity > threshold):InReset=1
  - [ ] grep `vkCreateImage` 在 sr_only_pipeline.cpp → 0
  - [ ] Stage 4/5/6 不出现在调用栈(用 ast_grep 验证)

  **QA Scenarios**:
  ```
  Scenario: 5 帧端到端 SR(happy [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. 启动 SrOnlyPipeline with sample-dlssconfig.json (5 frames)
      2. 阻塞等所有帧完成
      3. ls output/sr/ → 5 个 EXR
      4. 用 OpenEXR 验证每帧像素无 NaN/Inf
      5. progress 完成时 percent==1.0
    Evidence: .omo/evidence/task-36-sr-5frame.txt + 5 EXR

  Scenario: 帧 gap reset(happy edge)
    Tool: Catch2
    Steps:
      1. 路径 [0001, 0003, 0004](缺 0002)
      2. mock dlss_sr.evaluate 监听 InReset
      3. 第 1, 2 次 evaluate(0001, 0003)InReset=1(首帧 + gap),第 3 次(0004)= 0
    Evidence: .omo/evidence/task-36-gap-reset.txt

  Scenario: 相机切 reset(happy edge)
    Tool: Catch2
    Steps:
      1. fixture 5 帧,frame 3 的 matrix_world 与 frame 2 出现可被 cameraDiscontinuity 检测的差异(具体阈值在 T38 集成测试期 calibrate;此 scenario 用 mock cameraDiscontinuity 直接返回 true)
      2. evaluate 调用:第 3 帧 InReset=1
    Evidence: .omo/evidence/task-36-cam-cut.txt

  Scenario: 不调 Stage 4/5/6(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'TonemapPass::execute' src/pipeline/sr_only_pipeline.cpp → 0
      2. ast_grep 'DlssFgPass' src/pipeline/sr_only_pipeline.cpp → 0
      3. ast_grep 'InverseTonemapPass' src/pipeline/sr_only_pipeline.cpp → 0
    Evidence: .omo/evidence/task-36-no-fg-stages.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): SR-only end-to-end pipeline (Stage 1→2→7)`
  - Files: `src/pipeline/sr_only_pipeline.h`、`sr_only_pipeline.cpp`、`tests/test_sr_only_pipeline.cpp`

- [ ] 37. FG-only 主流程拼装(Stage 1 → 4 → 5 → 6 → 7)

  **What to do**:
  - 写 `src/pipeline/fg_only_pipeline.h` + `fg_only_pipeline.cpp`:
    - `class FgOnlyPipeline`:scale==1.0 但启用 FG 时(罕见用例,但管线对称要求支持)
    - 每帧:
      1. **Stage 1**:同 T36(prefetcher + channel mapper + depth/MV shader → render res GPU image)
      2. **Stage 4**:`TonemapPass.execute` scene-linear → display-ready(per dlssconfig.is_hdr 选 sRGB EOTF or PQ EOTF),pBackbuffer 在 render res(因为 scale==1.0)
      3. **Stage 5**:`DlssFgPass.evaluate` 多帧 eval 循环(multiplier-1 次 per input frame),输出 N 个插值帧
      4. **Stage 6**:每个输出帧 `InverseTonemapPass.execute` display-ready → scene-linear
      5. **Stage 7**:每个 input + 每个 interp 帧 → `AsyncFrameWriter.enqueue({Exr})`
      6. progress 更新(总输出帧 = 2N-1 for 2x, 4N-3 for 4x)
    - 输出帧重编号(per Q2 决议):`output_dir/0001.exr, 0002.exr, ..., 0(2N-1).exr`
    - `cameraDiscontinuity` 触发 FG reset

  **Must NOT do**:
  - 不要跑 SR(per 命名,FG-only)
  - 不要在 FG 之前跳过 tonemap(必须 display-ready)
  - 不要在 EXR 写入时保留 display-ready(EXR 必须 scene-linear,经 inverse tonemap 后再写)
  - 不要忘记多帧 eval 循环(2x 也是 1 次,4x 是 3 次)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: tonemap 边界 + FG 多帧 + 帧重编号,容易写错
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 6 sequential(T36 之后)
  - **Blocks**: 38
  - **Blocked By**: 26, 27, 28, 36(refactor 共享 Stage 1)

  **References**:
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §HDR(EOTF 必须)
  - 用户 Q2 决议:重编号
  - **WHY**:对称管线,即使罕见使用也必须正确实现以验证 Stage 4/5/6 编排

  **Acceptance Criteria**:
  - [ ] FgOnlyPipeline 5 帧 + 2x → 9 输出 EXR
  - [ ] 5 帧 + 4x → 17 输出(`4N-3 = 17`,若测试机支持)
  - [ ] 输出帧 scene-linear(经 inverse tonemap 后)
  - [ ] 重编号 0001..0009 连续
  - [ ] FG eval 调用次数 = 2x→4 次(N-1=4 input pairs × 1),4x→4×3=12 次
  - [ ] grep `DlssSrPass::evaluate` in fg_only_pipeline.cpp → 0(不调 SR)

  **QA Scenarios**:
  ```
  Scenario: 5 帧 FG 2x → 9 输出(happy [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. fixture 5 帧 + dlssconfig.is_hdr=true
      2. FgOnlyPipeline.run(multiplier=2)
      3. ls output/fg/ → 0001.exr ... 0009.exr
      4. 验证全部 scene-linear,无 NaN
    Evidence: .omo/evidence/task-37-fg-2x.txt + 9 EXR

  Scenario: HDR PQ round-trip 精度(happy)
    Tool: Catch2 [gpu]
    Steps:
      1. fixture 5 HDR 帧(像素 max value > 5.0 in scene-linear)
      2. 跑 FgOnlyPipeline
      3. 输出 EXR 像素 max 与输入相对误差 < 5%(经 PQ encode + FG + inverse PQ 损失允许)
    Evidence: .omo/evidence/task-37-hdr-precision.txt

  Scenario: 不调 SR(failure prevention)
    Tool: ast_grep
    Steps:
      1. ast_grep --lang cpp 'DlssSrPass' src/pipeline/fg_only_pipeline.cpp → 0
    Evidence: .omo/evidence/task-37-no-sr.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): FG-only end-to-end pipeline (Stage 1→4→5→6→7)`
  - Files: `src/pipeline/fg_only_pipeline.h`、`fg_only_pipeline.cpp`、`tests/test_fg_only_pipeline.cpp`

- [ ] 38. SR+FG 主流程拼装(Stage 1 → 2 → 4 → 5 → 6 → 7)

  **What to do**:
  - 写 `src/pipeline/sr_fg_pipeline.h` + `sr_fg_pipeline.cpp`:
    - 完整 7 阶段主管线
    - 每帧:
      1. **Stage 1**:同上(render res depth/MV)
      2. **Stage 2**:SR Eval(scene-linear render → scene-linear output)
      3. **Stage 4**:Tonemap+EOTF(LDR=sRGB EOTF only;HDR=ACES Filmic + PQ ST.2084)— scene-linear → display-ready
      4. **Stage 5**:FG Eval,**关键:depth/MV 用 Stage 1 的 render-res image(per FG §6 L545),不是 Stage 2/4 输出**
         - 多帧循环 multiplier-1 次
      5. **Stage 6**:Inverse Tonemap(每个 interp 帧 + real 帧)display-ready → scene-linear
      6. **Stage 7**:Async write(EXR/PNG/ProRes 422 HQ per T29 format matrix,若启用)
      7. progress + 预览帧(实时取最新 SR 输出帧推 EventChannel)
    - 输出帧重编号 0001..0(2N-1) 或 0(4N-3)
    - **核心 invariant**:depth/MV `PooledTextureHandle` 在 Stage 1 acquired,Stage 2 + Stage 5 共享同一 handle,Stage 7 写完后 release
  - 跨 task 集成测试 `tests/test_sr_fg_integration.cpp`(`[gpu]`):
    - 加载 fixture(60 帧 4K render 1080p Balanced,FG 2x 启用,is_hdr=true)
    - 跑完整管线
    - 输出 = 60 SR + 59 interp = 119 EXR + 1 ProRes MOV
    - **验证 EXR 输出范围合理**(无 NaN/Inf,scene-linear,中心像素与输入有合理相关性)
  - **共享 depth/MV invariant 自动验证**:在 Stage 5 evaluate 调用前,断言传给 FG 的 depth ImageView 与 SR 在 Stage 2 用的是同一个(handle id 比较)

  **Must NOT do**:
  - **不要为 FG 重新分配 depth/MV image**(必须复用 Stage 1)
  - 不要在 Stage 5 之后再跑 SR(管线方向锁定:SR → FG)
  - 不要在 SR 之前 tonemap(违反 §3.1)
  - 不要让 FG 接到 Stage 2 的 scene-linear 输出(必须经 Stage 4 的 display-ready)
  - 不要让 EXR 写入是 display-ready(必须经 Stage 6 inverse 后 scene-linear)
  - 不要在 SR + FG 间数据流中 vkCreateImage(全 TexturePool)

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 7 阶段编排;invariants 多;依赖前 6 个 task 全部正确
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 6 sequential(T37 之后)
  - **Blocks**: 39-43(docs 引用此管线)
  - **Blocked By**: 36, 37(共享 Stage 1 / Stage 7 抽象)

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3.1(SR before tonemap)
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §1(管线 SR → FG),§6 L545(depth/MV 共享)
  - **WHY**:这是项目终极 deliverable,F1-F4 reviewer 在此聚焦验证

  **Acceptance Criteria**:
  - [ ] SrFgPipeline 60 帧 4K Balanced FG 2x → 119 EXR + 1 ProRes MOV
  - [ ] 输出 EXR scene-linear,无 NaN/Inf
  - [ ] FG depth/MV 与 SR 共享 ImageView(handle id 一致,断言通过)
  - [ ] 管线方向不可逆(grep 验证 Stage 5 不在 Stage 2 之前调用)
  - [ ] 完整运行总耗时 < 5 min(在 RTX 4090 上,4K Balanced FG 2x 60 帧)
  - [ ] 进度更新连续(每帧 percent 严格递增)
  - [ ] 预览帧 EventChannel emit ≥ 30 次(每 2 帧推一帧)

  **QA Scenarios**:
  ```
  Scenario: 60 帧 4K SR+FG 2x 端到端(happy critical [gpu])
    Tool: Catch2 [gpu]
    Steps:
      1. fixture: 60 帧 1080p Balanced is_hdr=true
      2. SrFgPipeline.run(multiplier=2)
      3. ls output/sr_fg/ → 119 EXR
      4. 验证全部 scene-linear,无 NaN
      5. ls output/sr_fg/video.mov → 存在
      6. ffprobe video.mov → codec=prores, profile=3, frames=119
      7. 总耗时 < 300s
    Evidence: .omo/evidence/task-38-60frame-4k.log + 119 EXR + 1 MOV

  Scenario: depth/MV 共享 invariant(failure prevention critical)
    Tool: Catch2 + assertion
    Steps:
      1. mock SR 与 FG eval,记录传入的 depth ImageView 句柄
      2. 比较两次记录:必须同一个 handle
      3. 任何不同 → 失败 + LOG_ERROR
    Evidence: .omo/evidence/task-38-shared-depth.txt

  Scenario: 管线方向(failure prevention)
    Tool: ast_grep
    Steps:
      1. 检查 sr_fg_pipeline.cpp 的 dispatch sequence
      2. 顺序必须是 Stage 1, 2, [3], 4, 5, 6, 7
      3. 不允许 Stage 5 在 Stage 2 之前
    Evidence: .omo/evidence/task-38-pipeline-order.txt

  Scenario: HDR PQ + scene-linear 双向(happy)
    Tool: Catch2 [gpu]
    Steps:
      1. is_hdr=true 输入
      2. 验证 Stage 4 用 PQ EOTF
      3. Stage 6 用 inverse PQ
      4. EXR 写入是 scene-linear(读回最大像素 > 1.0,因 HDR)
    Evidence: .omo/evidence/task-38-hdr-roundtrip.txt

  Scenario: 总输出帧数(happy)
    Tool: Bash
    Steps:
      1. 60 帧输入 + 2x → ls output/sr_fg/*.exr | wc -l == 119
      2. 60 帧 + 4x → 237 EXR(若 RTX 40 测试机)
    Evidence: .omo/evidence/task-38-frame-count.txt
  ```

  **Commit**: YES
  - Message: `feat(pipeline): SR+FG end-to-end (Stage 1→2→4→5→6→7) with shared depth/MV`
  - Files: `src/pipeline/sr_fg_pipeline.h`、`sr_fg_pipeline.cpp`、`tests/test_sr_fg_integration.cpp`

- [ ] 39. README.md v2

  **What to do**:
  - 写根目录 `README.md`(英文为主,简短中文摘要):
    - **What**:DLSS Compositor v2 — Blender Cycles EXR 离线批渲过 NVIDIA DLSS SR + FG
    - **Stack**:Blender 5.1+ extension + Flutter Windows GUI + C++ Vulkan/NGX engine
    - **Requirements**:Win10/11、NVIDIA RTX(Turing+ 跑 SR、Ada+ 跑 FG)、driver 535+、Vulkan SDK 1.3+、VS2022、CMake 3.20+
    - **Quick Start**(5 步):
      1. Clone + submodule update
      2. 装 vcpkg + bootstrap
      3. cmake build
      4. flutter build windows
      5. 装 Blender extension + 配置 quality + 渲染 + 启动 GUI
    - **Features**:SR 1x/2x/3x 超分 + FG 2x/3x/4x 帧生成 + 中英双语用户手册
    - **Pipeline diagram**(ASCII or mermaid):Blender → dlssconfig.json → Flutter GUI → C++ engine → EXR/ProRes
    - **Links**:`docs/build_guide.md`、`docs/blender_export_guide.md`、`docs/gui_guide.md`、`docs/cli_reference.md`、`docs/user_manual.html`
    - **Documentation accuracy**:**绝不写"FG internally clamps to 10 nits"等假话**(per audit);只描述 DLSS doc 实际要求(SR 在 tonemap 前/FG 在 tonemap 后/EOTF 必须)
    - **License**:MIT
  - 写 `LICENSE`(MIT)
  - 写 `AGENTS.md`(v2 版本):简短指引未来 agent 在 v2 codebase 的 conventions(spdlog 用 stderr、不写 stdout、NGX 调用必须有 §引用注释、每 task 有 Agent QA scenarios)

  **Must NOT do**:
  - **绝不写假话**(per v1 README 的 "10-nit clamp" 教训)
  - 不要列已切的 features(DNxHR / 4444 / DX12 / RR / NIS)
  - 不要在 README 写 implementation 细节(留给 build_guide / cli_reference)
  - 不要全中文或全英文(目标读者是国际开发者,英文为主)

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: 文档,模板化但要严格事实
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 7
  - **Blocks**: 无
  - **Blocked By**: 38(管线 ready 才能写 quick start)

  **References**:
  - `docs/dlss-refs/DLSS_Programming_Guide_Release.md` §3(SR 真实要求)
  - `docs/dlss-refs/DLSS_FG_Programming_Guide.md` §HDR(EOTF,**不是** clamp)
  - 用户决议 round 2:LDR 项目支持 PNG + EXR + ProRes 422 HQ;HDR 项目支持 EXR + ProRes 422 HQ;扩展格式(DNxHR/4444/JPEG/H.264/H.265/AV1/AVIF/WebP)在 "Future Extensions" 章节作未来计划
  - **WHY**:文档错会再骗 agent;v1 README 的 10-nit 假说就是这么传染开的

  **Acceptance Criteria**:
  - [ ] README.md ≤ 200 行
  - [ ] 不含 "10 nit" / "10-nit" / "clamp" 在描述 FG 行为时
  - [ ] 不在"Quick Start / Supported Formats / Implementation"小节宣称 DX12 / RR / NIS / Electron(扩展项 DNxHR/H264/H265/AV1/AVIF/JPEG 等可在"Future / Roadmap"小节列为未来计划,但不能宣称 v2 已实现)
  - [ ] Quick Start 5 步可被新人按步骤跑通
  - [ ] AGENTS.md 列出至少 5 条 v2 conventions

  **QA Scenarios**:
  ```
  Scenario: README 不含假话(failure prevention critical)
    Tool: grep
    Steps:
      1. grep -iE "10[- ]?nit|clamp.*to.*nit" README.md → 0
      2. grep -iE "DX12|electron|ray reconstruction|NIS\b" README.md → 0(已切的功能不能宣称 v2 支持;DNxHR/H.264 等扩展项允许在 "Future Extensions" 章节提及)
    Evidence: .omo/evidence/task-39-no-false-claims.txt

  Scenario: Quick Start 可执行(integration)
    Tool: Bash
    Steps:
      1. 用 README quick start 步骤 + 全新 Windows VM
      2. 5 步全过 + GUI 启动 + dlssconfig 选择成功
    Evidence: .omo/evidence/task-39-quick-start-fresh-vm.log

  Scenario: AGENTS.md conventions(happy)
    Tool: Bash
    Steps:
      1. wc -l AGENTS.md → ≥ 30
      2. grep -E "spdlog|stdout|NGX.*§|Agent QA" AGENTS.md → ≥ 5 行
    Evidence: .omo/evidence/task-39-agents-md.txt
  ```

  **Commit**: YES
  - Message: `docs: README v2 + LICENSE + AGENTS.md`
  - Files: `README.md`、`LICENSE`、`AGENTS.md`

- [ ] 40. docs/build_guide.md(完整 build flow)

  **What to do**:
  - 写 `docs/build_guide.md`:
    - 章节:
      1. **Prerequisites**:VS2022 (with C++ workload + MSVC v143)、CMake 3.20+、Vulkan SDK 1.3+(含 glslc)、vcpkg(bootstrap 步骤)、Flutter 3.22+、Python 3.11(for blender pytest)、Git with LFS
      2. **Clone**:`git clone --recurse-submodules`(强调 submodule)
      3. **vcpkg setup**:bootstrap + 设 `VCPKG_ROOT` env
      4. **CMake configure + build**:`cmake -B build --preset x64-windows-msvc-release` + `cmake --build build --config Release`
      5. **CLI 测试**:`build/Release/dlss-compositor-cli.exe --help` / `--self-check`
      6. **Flutter build**:`cd gui && flutter pub get && flutter build windows --release`
      7. **Run**:启动 `build/windows/x64/runner/Release/dlss_compositor_gui.exe`
      8. **Debug build + VSCode F5**:引用 `docs/debug_setup.md`
      9. **Troubleshooting**:常见错误(submodule not initialized / vcpkg not found / vulkan SDK 老 / NGX init failed)
    - 每条命令都给完整 PowerShell snippet
  - 在文末嵌一个 mermaid 流程图说明 build 依赖关系

  **Must NOT do**:
  - 不要假设用户已熟悉 vcpkg / Flutter
  - 不要省略 Vulkan SDK 安装(很容易漏)
  - 不要让 quick start 假设 dev 环境已配好

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: 文档
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 7
  - **Blocks**: 无
  - **Blocked By**: 38

  **References**:
  - vcpkg manifest mode docs
  - Vulkan SDK 1.3.x install docs
  - **WHY**:build flow 太多步骤,新手必须有 step-by-step 指南

  **Acceptance Criteria**:
  - [ ] docs/build_guide.md ≥ 200 行 + 9 章节
  - [ ] 每个 PowerShell snippet 可复制执行
  - [ ] 在全新 Windows 11 VM 跑通整个 guide(integration test)

  **QA Scenarios**:
  ```
  Scenario: 全新 VM 按 guide 跑通(integration)
    Tool: Bash + manual VM
    Steps:
      1. 全新 Win11 VM
      2. 严格按 guide 逐条执行
      3. 最终 GUI 启动成功
    Evidence: .omo/evidence/task-40-vm-build-flow.log

  Scenario: 文档完整性(happy)
    Tool: Bash
    Steps:
      1. wc -l docs/build_guide.md → ≥ 200
      2. grep -c "^##" docs/build_guide.md → ≥ 9(章节数)
    Evidence: .omo/evidence/task-40-doc-completeness.txt
  ```

  **Commit**: YES
  - Message: `docs: build guide with vcpkg + flutter + VS2022 step-by-step`
  - Files: `docs/build_guide.md`

- [ ] 41. docs/blender_export_guide.md(新插件操作流程)

  **What to do**:
  - 写 `docs/blender_export_guide.md`(英文 + 关键截图):
    - **Install extension**:Blender 5.1+ → Edit > Preferences > Get Extensions > Install from Disk
    - **Configure project**:打开 `Render Properties > DLSS Compositor` panel
    - **Set Output Resolution**:在 Blender **标准** `Render Properties > Format > Resolution X/Y` 设置(**不在 DLSS 插件 panel 中**,因 Blender 自带)
    - **Choose Quality Mode**:在 DLSS 插件 panel 的横排 toggle 选 6 选项之一,推荐:
      - **DLAA**:max 质量,1×(无 SR)
      - **Quality**:1.5×
      - **Balanced**:1.724×
      - **Performance**:2×
      - **UltraPerformance**:3×
    - **Click "Apply Settings"**:自动启用 Combined/Depth/Vector pass + 创建 Compositor File Output 节点 + 强制 Cycles denoise + 强制 OCIO Raw + view transform Override(**不**改 `resolution_percentage` — 那是 plugin handler 在 render_init 时透明做的)
    - (Configure All Passes 按钮已合并入 Apply Settings,无需单独点击)
    - **Render animation**:`Ctrl+F12`(handler 静默写 per-frame `frames/<N>.cam.json`)
    - **Render complete**:自动 merge 到 `dlssconfig.json`
    - **Open in Flutter GUI**:`dlss_compositor_gui.exe` → Open dlssconfig.json
    - **Gotchas**(端用户常见坑):
      - **不要中途改 Quality / Resolution**(history invalid,必须重渲)
      - **必须开 Cycles denoise**(DLSS 训练数据是去噪后的)
      - **OCIO 设置改了 → 必须 reapply Configure All Passes**(强制 Raw)
      - **HDR 项目用 Display Device = Rec.2100-PQ**(`is_hdr=true` 自动)
    - **Troubleshooting**:输出 EXR 颜色不对 → 检查 OCIO;jitter 序列错 → 重 install 插件
  - 截图:Render Properties 面板 + Quality toggle + Apply 后的 percentage 显示

  **Must NOT do**:
  - 不要省略 OCIO 强制 Raw 的解释(关键 gotcha)
  - 不要假设用户懂 Halton jitter
  - 不要列 v1 的 export camera 手动按钮(v2 自动)

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: 文档 + 截图
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 7
  - **Blocks**: 无
  - **Blocked By**: 11, 12, 13, 14, 15

  **References**:
  - 用户决议:Q1 quality → percentage 自动绑定;Q3 不写 cut_after
  - Cycles denoise docs
  - **WHY**:Blender 用户是核心,导出错则全管线无效

  **Acceptance Criteria**:
  - [ ] docs/blender_export_guide.md ≥ 100 行 + 6 章节
  - [ ] ≥ 3 张截图(Render Properties 面板、Quality toggle、Apply 后 state)
  - [ ] Gotchas 列举至少 4 条
  - [ ] 真 Blender 5.1+ 中按 guide 跑通

  **QA Scenarios**:
  ```
  Scenario: 真 Blender 端到端(integration)
    Tool: Manual + screenshot
    Steps:
      1. 全新 Blender 5.1,装扩展,打开测试 .blend
      2. 按 guide 步骤(set output → quality → configure → render 5 frames)
      3. 验证 dlssconfig.json 生成 + frames 完整
    Evidence: .omo/evidence/task-41-blender-walkthrough.log + screenshots

  Scenario: 文档 gotchas 完整(happy)
    Tool: grep
    Steps:
      1. grep -c "^### Gotcha\|^- \*\*Gotcha\*\*\|denoise.*must\|OCIO.*Raw" docs/blender_export_guide.md → ≥ 4
    Evidence: .omo/evidence/task-41-gotchas.txt
  ```

  **Commit**: YES
  - Message: `docs: blender export guide with quality/OCIO/denoise gotchas`
  - Files: `docs/blender_export_guide.md`、`docs/img/blender_*.png`

- [ ] 42. docs/gui_guide.md + docs/cli_reference.md

  **What to do**:
  - 写 `docs/gui_guide.md`:
    - 章节:
      1. **Launch**:双击 `dlss_compositor_gui.exe`
      2. **Status bar**:版本 + GPU + FG max
      3. **Open dlssconfig.json**:Win32 file dialog,选 Blender 导出的 json
      4. **Configure task**:preset / FG toggle / FG 倍率 / 输出格式 / 输出目录
      5. **Start render**:进度条 + 实时预览帧(2-4 fps)
      6. **Done**:统计面板 + Open Folder
      7. **Troubleshooting**:dlssconfig 无效错误 / FG 倍率灰显原因 / 预览帧不动
    - 截图:status bar、task config panel、detail panel(预览帧 + 进度)、done state
  - 写 `docs/cli_reference.md`:
    - **简化的 CLI**(per Metis 决议:大部分配置已在 dlssconfig.json):
      - `dlss-compositor-cli.exe --config <dlssconfig.json> [options]`
      - `--preset <auto|J|K|L|M>`(默认 auto)
      - `--fg <none|2x|3x|4x>`(默认 none)
      - `--output-dir <path>`
      - `--output-format <png|exr|prores>`(LDR 默认 png;HDR 默认 exr;HDR + png 组合 reject)
      - `--output-template <pattern>`(默认 `frame_####.<ext>`,占位符 `####` / `{frame:04d}` / `##` for version)
      - `--overwrite-mode <skip|overwrite|auto-version>`(默认 overwrite)
      - `--rpc-stdio`(JSON-RPC mode,for Flutter plugin fallback / 自动化)
      - `--memory-budget <GB>`(默认 8)
      - `--self-check`(GPU + NGX 可用性 banner)
      - `--version` / `--help`
    - 每个 flag 的解释 + 示例命令
    - **明确不支持**的:--scale(由 dlssconfig 决定)、--quality(由 dlssconfig 决定)、--encode-video(用 ProRes 通过 --output-format)
    - JSON-RPC mode 说明:method list、Content-Length framing、event notifications

  **Must NOT do**:
  - 不要列 v1 的旧 flags(`--scale`、`--inspect-exr` 等)
  - 不要在 cli_reference "Implemented Flags / Supported Formats" 章节列 DNxHR / 4444 选项;允许在专门的 "Future Extensions / Roadmap" 章节作未来计划提及

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: 文档
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 7
  - **Blocks**: 无
  - **Blocked By**: 31, 33, 34

  **References**:
  - 用户决议:CLI 简化,大部分配置从 dlssconfig 来
  - **WHY**:GUI 用户面 + CLI 用户面分别有需求

  **Acceptance Criteria**:
  - [ ] docs/gui_guide.md ≥ 80 行 + ≥ 4 张截图
  - [ ] docs/cli_reference.md ≥ 100 行 + 所有 flag 列出 + 示例
  - [ ] grep `--scale` / `--quality` in cli_reference → 0(已废)
  - [ ] 不在"Implemented / Supported"章节宣称 DNxHR/4444 等扩展项;`docs/cli_reference.md` 必须含 "Future Extensions" 章节列出 7 个 enum 占位

  **QA Scenarios**:
  ```
  Scenario: GUI guide + CLI reference 完整(happy)
    Tool: Bash
    Steps:
      1. wc -l docs/gui_guide.md → ≥ 80
      2. wc -l docs/cli_reference.md → ≥ 100
      3. grep -c "^- \`--" docs/cli_reference.md → ≥ 8(flag 数)
    Evidence: .omo/evidence/task-42-doc-shape.txt

  Scenario: 不列废弃 flag(failure prevention)
    Tool: grep
    Steps:
      1. grep -E -- "--scale\b|--quality\b|--inspect-exr" docs/gui_guide.md docs/cli_reference.md → 0(已废 flag;扩展格式可在"Future Extensions"章节列出)
    Evidence: .omo/evidence/task-42-no-deprecated.txt
  ```

  **Commit**: YES
  - Message: `docs: GUI guide + CLI reference (simplified flags)`
  - Files: `docs/gui_guide.md`、`docs/cli_reference.md`、`docs/img/gui_*.png`

- [ ] 43. docs/user_manual.html(中英双语单文件)

  **What to do**:
  - 写 `docs/user_manual.html` — **单 HTML 文件**,内嵌 CSS,无构建依赖,双击浏览器打开
  - 结构:
    - 顶部 language tab(中文 / English)用 vanilla JS 切换 div display
    - 内容章节(中英两套同步):
      1. **What is DLSS Compositor?**(2 段简介)
      2. **Quick Start / 快速开始**(5 步 + 截图,与 README quickstart 对应)
      3. **Workflow / 工作流程**(Blender → render → GUI → SR/FG → output 的端到端图)
      4. **Settings explained / 设置说明**:
         - Quality Mode 含义 + DLSS 推荐何时用哪个
         - SR vs FG vs SR+FG 各自适合的场景
         - HDR vs LDR 项目区别
      5. **Notes & Gotchas / 注意事项**(端用户必读):
         - **DLSS 导出行为与 Blender 设置绑定**(用户原话):quality mode 决定 `resolution_percentage` + jitter phase 数 + Halton 序列。**改 quality 后必须重渲整段**,否则历史帧 jitter 与新设置不匹配 → SR 退化
         - Cycles denoise 必须开
         - FG 需要 `dlssconfig.json` 的 per-frame camera matrices,SR 单跑不需要
         - **不要中途改 quality / resolution**(history 失效)
         - HDR 项目(`is_hdr=true`)输出格式:**EXR(scene-linear, default)+ ProRes 422 HQ**;LDR 项目(`is_hdr=false`)输出格式:**PNG 16-bit(default)+ EXR + ProRes 422 HQ**;DNxHR / H.264 / H.265 / AV1 / AVIF / JPEG / WebP / ProRes 4444 XQ 列在 "Future Extensions" 章节(v2 不实现)
         - Debug overlay 在 headless 不可用(只 dev DLL + 键盘 hotkey)
      6. **FAQ / 常见问题**:
         - "NGX feature unavailable" → driver 旧 / GPU 不兼容
         - "dlssconfig schema invalid" → Blender 端 schema 校验,看具体字段
         - "FG 倍率灰显" → GPU 不支持(RTX 30 及以下不支持 FG)
         - "Preview frame 不动" → 看日志,可能 FFmpeg 错误
         - "输出 EXR 颜色不对" → 检查 OCIO Raw 设置
      7. **Troubleshooting / 故障排查**(简要)
    - 末尾:
      - **Embedded markdown**(`<details><summary>AI-readable markdown</summary><pre>...</pre></details>`):整个文档的 markdown 版,让 AI 能直接读
      - 版本 + 链接到完整 docs(build_guide / cli_reference / GitHub)
  - 内嵌 CSS:
    - simple, light + dark mode auto(via `prefers-color-scheme`)
    - language tab 在右上角
    - 章节 sticky TOC(可选)
  - 文件大小目标 < 500KB(无外部 CDN / fonts)

  **Must NOT do**:
  - 不要外链 CSS / JS(必须单文件)
  - 不要使用 build 工具(没人愿意装 webpack 看 user manual)
  - 不要把内容只英文或只中文(用户明确要求双语)
  - 不要列已切的 features
  - 不要使用 emoji **过度**(端用户文档严肃)

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: 文档,跨语言
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 7
  - **Blocks**: 无
  - **Blocked By**: 38, 39, 40, 41, 42(汇总所有信息)

  **References**:
  - 用户原话:中英双语 + AI 也能看(参考 session-knowledge-html skill 理念)
  - User decisions: v2 实现仅 EXR + PNG (LDR) + ProRes 422 HQ;DNxHR/H264/H265/AV1/AVIF/JPEG/4444 留作未来扩展(在 docs Future 章节提及)
  - **WHY**:端用户唯一的"非开发者"入口;必须自包含

  **Acceptance Criteria**:
  - [ ] docs/user_manual.html 单文件 ≤ 500KB
  - [ ] 双击在 Chrome / Edge / Firefox 打开 → 渲染正常
  - [ ] 顶部 language tab 切换工作
  - [ ] 中英文内容对等(章节数相同)
  - [ ] 末尾 `<details>` 含完整 markdown,可被 AI 提取
  - [ ] grep "10 nit" → 0(误传 v1 假说);"Future Extensions"章节可提 DNxHR/H264/H265/AV1/AVIF 但不能宣称 v2 已实现
  - [ ] Gotchas 章节含至少 5 条(包括用户提的"DLSS 导出行为绑定")

  **QA Scenarios**:
  ```
  Scenario: 浏览器打开 + 双语切换(happy critical)
    Tool: chrome-devtools-mcp 或 Manual
    Steps:
      1. 用 Playwright 打开 docs/user_manual.html (file:// URL)
      2. 验证默认中文 div 可见,英文 hidden
      3. 点击 English tab → 英文 div 可见,中文 hidden
      4. 截图两态
    Evidence: .omo/evidence/task-43-dual-lang-screenshots/

  Scenario: 文件大小约束(happy)
    Tool: Bash
    Steps:
      1. ls -l docs/user_manual.html → ≤ 500KB
      2. grep -c "https://" docs/user_manual.html → 0(无外链)
      3. grep -c "<link rel" docs/user_manual.html → 0(无外部 CSS)
    Evidence: .omo/evidence/task-43-self-contained.txt

  Scenario: AI-readable markdown 嵌入(happy)
    Tool: Bash + sed
    Steps:
      1. sed -n '/<details>/,/<\/details>/p' docs/user_manual.html | grep -c "^# " → ≥ 7(章节)
      2. 提取 markdown,字数 > 1500(中英两套合计)
    Evidence: .omo/evidence/task-43-embedded-md.txt

  Scenario: Gotchas 关键条目存在(failure prevention)
    Tool: grep
    Steps:
      1. grep -i "quality.*resolution_percentage.*绑定\|jitter phase\|cycles denoise" docs/user_manual.html → ≥ 1
      2. grep -i "EXR.*ProRes 422 HQ" docs/user_manual.html → ≥ 1
    Evidence: .omo/evidence/task-43-gotchas-key.txt

  Scenario: 不列废弃 features(failure prevention)
    Tool: grep
    Steps:
      1. grep -iE "DX12|electron|10[- ]?nit|ray reconstruction" docs/user_manual.html → 0(已切的功能不能宣称;扩展项 DNxHR/H264 等可在 Future Extensions 提及)
    Evidence: .omo/evidence/task-43-no-deprecated.txt
  ```

  **Commit**: YES
  - Message: `docs: bilingual single-file user manual (HTML + embedded markdown)`
  - Files: `docs/user_manual.html`

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents 并行执行。**全部 APPROVE** 才能向用户呈递。**用户必须明确 "okay" 才完成,Prometheus 不自动标完成 F1-F4**。

- [ ] F1. **Plan Compliance Audit** — `oracle`

  **What to do**: 端到端读 `.omo/plans/dlss-compositor-v2.md`。对每个 Must Have:验证实现存在(读文件 / curl / 跑命令)。对每个 Must NOT Have:全 codebase 搜禁用模式 — 找到则 REJECT 并附 file:line。验证 `.omo/evidence/task-{N}-*.{ext}` evidence 文件齐全。对照 plan 检查所有 deliverables 是否产出。

  **Specific checks**:
  - 全代码搜 `// DLSS §` 引用注释,确认每个 `NVSDK_NGX_*` 调用旁都有
  - 搜 `InMVScale.*=.*[02]\.0` / `MVScaleX.*=.*[02]\.0` 确认 SR/FG MV scale 都 = 1.0
  - 搜 `InReset.*=.*true.*for|every|each` 确认无每帧 reset
  - 搜 `InFrameTimeDeltaInMsec.*=.*33` 确认无硬编码
  - 搜 `pNoPostProcessingColor` 确认 0 处引用(310.6.0 已删)
  - 搜 `cout|printf|fprintf\(stdout` 在 src/ 下 → 报错(stdout 仅 RPC)
  - 验证 schema `core/dlssconfig.schema.json` 存在且 T6/T15/T32 都加载它
  - 验证 v2 分支 git log 不含 v1 文件复活(检查 src/ gui/ 旧路径)

  **Recommended Agent Profile**:
  - **Category**: 不适用 — F1 直接调 `subagent_type=oracle`(read-only consultant for plan compliance verification)
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES(与 F2-F4 并行)
  - **Parallel Group**: Wave Final
  - **Blocks**: 用户 okay
  - **Blocked By**: 1-43 全部完成(注:T25 已删留 gap,实际 active = 42)

  **Acceptance Criteria**:
  - [ ] 验证 ≥ 31 个 Must Have items(plan §Work Objectives)逐项 grep / 跑命令通过
  - [ ] 验证 ≥ 20 个 Must NOT Have items 全 codebase 0 命中
  - [ ] 42 个 task(T25 CAS 已删,留 gap)各自的 Acceptance Criteria 全部 ✅
  - [ ] `.omo/evidence/task-*` 文件 ≥ 100 个(每 task 平均 2-3 evidence)
  - [ ] 输出明确 VERDICT 字符串

  **QA Scenarios**:
  ```
  Scenario: 全 Must Have/Must NOT Have grep 通过(happy)
    Tool: oracle agent
    Steps:
      1. oracle 读 plan + 全 codebase
      2. 对每个 Must Have 输出 ✅/❌ + 证据
      3. 对每个 Must NOT Have 输出 ✅/❌ + grep 结果
    Expected Result: 全部 ✅,VERDICT: APPROVE
    Evidence: .omo/evidence/final-f1-compliance-audit.md

  Scenario: 抓到 Must NOT Have 违规(failure)
    Tool: oracle
    Steps:
      1. 故意在某 task 实现里加 `InMVScaleX = 2.0f`
      2. F1 应 grep 命中 + REJECT + 附 file:line
    Expected Result: VERDICT: REJECT
    Evidence: .omo/evidence/final-f1-detect-regression.md
  ```

  **Output**: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | Evidence [N/N] | VERDICT: APPROVE/REJECT`

- [ ] F2. **Code Quality Review** — `unspecified-high`

  **What to do**: 跑 `cmake --build build --config Release`(0 warning)、`ctest --output-on-failure`(全过)、`cd gui && flutter analyze && flutter test && flutter test integration_test`(全过)、`cd blender && python -m pytest`(全过)。审查所有改动文件:`as any` / `@ts-ignore` / 空 catch / `console.log` / 注释掉的代码 / 未用 import。AI slop check:过度注释、过度抽象、generic 命名(data/result/item/temp)、过度 docstring。

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 跨 C++/Dart/Python 全栈 lint + 主观 AI slop 审查需要工程师级判断
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave Final
  - **Blocks**: 用户 okay
  - **Blocked By**: 1-43(T25 gap,42 active)
  - **Wave 4** alignment: 4a [24, 26, 29] / 4b [27, 28]
  - [ ] `ctest` 全部 pass(包括 GPU 标记测试)
  - [ ] `flutter analyze` 0 issue
  - [ ] `flutter test` + `integration_test` 全过
  - [ ] `pytest blender/tests/` 全过
  - [ ] AI slop count ≤ 5(每 1000 LOC)

  **QA Scenarios**:
  ```
  Scenario: 全栈 build + test pass(happy)
    Tool: Bash
    Steps:
      1. 跑 cmake build → 0 warn
      2. 跑 ctest → all pass
      3. 跑 flutter analyze + test → 0 issue
      4. 跑 pytest → all pass
    Evidence: .omo/evidence/final-f2-build-test.log

  Scenario: AI slop 检测(failure prevention)
    Tool: ast_grep + grep
    Steps:
      1. grep -rE "// TODO|// XXX|// HACK" src/ → 报告数量
      2. ast_grep 'std::vector<$T>$$$Result|std::vector<$T>$$$data' → flag generic namings
      3. 数量 ≤ 5/1000 LOC
    Evidence: .omo/evidence/final-f2-slop-check.txt
  ```

  **Output**: `Build [PASS/FAIL] | Lint [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [ ] F3. **Real End-to-End QA(agent-executed)** — `unspecified-high` (+ `playwright` skill if needed for headless screenshot capture)

  > **命名澄清**:F3 名字"Real Manual QA"是历史遗留 — 实际是 **agent-executed 端到端 QA**(per Verification Strategy "ZERO HUMAN INTERVENTION"),agent 用 playwright / chrome-devtools-mcp / Bash 自动跑全流程 + 截图 evidence,**不需要人工点击**。任何"Manual via VSCode"步骤都是 agent 用 GUI automation tool 模拟。

  **What to do**:
  从干净 state 开始,执行每个 task 的 QA scenarios — 严格按步骤、捕获 evidence。测试跨 task 集成:
  - **端到端 SR**: 加载样例 Blender 场景 → 装插件 → 选 Balanced → 渲染 10 帧 → `dlss-compositor-cli.exe --config dlssconfig.json --output-dir out/`(`scale` 由 dlssconfig 决定,**v2 CLI 无 `--scale` flag**)→ 输出 10 个 EXR(2x 分辨率因 dlssconfig.quality=Balanced,无 NaN/Inf)
  - **端到端 SR+FG**: 同上 + 启用 FG 2x(`--fg 2x`)→ 输出 19 个 EXR(`2N-1`)+ 1 个 ProRes 422 HQ(`--output-format prores`)
  - **Flutter GUI 端到端**: 启动 `dlss-compositor-gui.exe` → 选 dlssconfig.json → 配 quality + FG 2x → Start → 预览帧 ≥ 2 fps → 进度更新 → 完成统计
  - **Edge case**: 空目录、缺帧序列、单帧 FG(应错误退出 code 5)、driver too old 模拟、quality mid-sequence change 检测
  - **VSCode F5 双调试**: launch.json compound → Dart 断点 + C++ 断点都命中

  Evidence 存 `.omo/evidence/final-qa/`(screenshots / 输出 EXR thumbnail / 命令日志 / pixel hash)。

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: 端到端 manual QA 需要协调 Blender + Flutter + CLI + GPU 全套环境
  - **Skills**: [`playwright`]
    - `playwright`: Flutter GUI 截图 / 交互验证(虽然 Flutter 不是 web,但 chrome-devtools-mcp 可截 desktop window)

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave Final
  - **Blocks**: 用户 okay
  - **Blocked By**: 1-43

  **Acceptance Criteria**:
  - [ ] 5 大端到端测试场景全过
  - [ ] 5 个 edge case 全过
  - [ ] VSCode 双调试断点命中 evidence(screenshot)
  - [ ] `.omo/evidence/final-qa/` 至少 20 个 evidence files(screenshots + logs + EXR samples)

  **QA Scenarios**:
  ```
  Scenario: 完整端到端真渲染(happy critical)
    Tool: Bash + Blender + Flutter + GPU
    Steps:
      1. 全新 Win11 测试机 + RTX 40 GPU
      2. 装 Blender 5.1+ 扩展
      3. 跑 build/setup
      4. 渲染 10 帧 fixture 场景
      5. 启动 GUI + Start SR+FG 2x
      6. 验证输出 19 EXR + 1 MOV
    Expected Result: 全过,捕获 evidence
    Evidence: .omo/evidence/final-qa/full-e2e/

  Scenario: VSCode 双调试可命中(happy)
    Tool: VSCode + manual
    Steps:
      1. F5 → compound launch
      2. Dart breakpoint 命中
      3. C++ breakpoint 命中
      4. 截图证据
    Evidence: .omo/evidence/final-qa/vscode-debug.png
  ```

  **Output**: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | Debug [PASS/FAIL] | VERDICT`

- [ ] F4. **Scope Fidelity Check** — `deep`

  **What to do**:
  对每个 1-43 task(T25 已删,留 gap):读 "What to do"、读实际 git diff(`git log v2/main --oneline` + `git diff base..HEAD`)。验证 1:1 对齐 — spec 列的全建了(无遗漏);spec 没列的没建(无 scope creep)。检查 "Must NOT do" 合规。检测 cross-task 污染(task N 改了 task M 的文件)。标记 unaccounted changes。

  **Specific checks**:
  - 验证 `src/` 不含 DX12 / Streamline / NIS / RR 代码
  - 验证 `gui/` 不含 Electron / React 残留
  - 验证 `blender/` 用 Halton(2,3) base=8 不是 base=1
  - 验证 `docs/user_manual.html` 含中英双栏 + 嵌 markdown
  - 验证 v2 实现期视频编码器 = ProRes 422 HQ 唯一(扩展项 H.264/H.265/AV1/DNxHR/4444 enum 占位但未注册到 EncoderRegistry)
  - 验证 v2 image 编码器 = EXR(scene-linear)+ Png16(LDR)两个;无 JPEG/AVIF/WebP/TIFF

  **Recommended Agent Profile**:
  - **Category**: `deep`
    - Reason: 跨 42 active task(T25 已删,留 gap)git diff 比对,需要深度推理和模式识别
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave Final
  - **Blocks**: 用户 okay
  - **Blocked By**: 1-43

  **Acceptance Criteria**:
  - [ ] 42 active task(T25 已删)每个都有 diff 对应 + 对齐验证
  - [ ] 0 cross-task 污染
  - [ ] 0 scope creep(spec 外 LOC < 1%)
  - [ ] 全 `src/` `gui/` `blender/` `docs/` grep 5 项 specific check 全 ✅

  **QA Scenarios**:
  ```
  Scenario: Scope 1:1 对齐(happy)
    Tool: deep agent
    Steps:
      1. 对每个 task 1-43:列 spec deliverables + 实际 git diff files
      2. 验证 spec 列的全在 diff 里
      3. 验证 diff 里无 spec 没列的(超 scope)
    Expected Result: 43/43 compliant
    Evidence: .omo/evidence/final-f4-scope-fidelity.md

  Scenario: 检测 scope creep(failure prevention)
    Tool: deep agent
    Steps:
      1. 故意在 v2 加一个 NIS 实现文件
      2. F4 必须标 "scope creep detected: src/dlss/nis_*.cpp"
    Evidence: .omo/evidence/final-f4-detect-creep.md
  ```

  **Output**: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | Scope Creep [N detected] | VERDICT`

---

## Commit Strategy

每个 task 一次 atomic commit(除非显式合并组,如 23 实证 sanity test 与主实现同 commit)。

- **commit message 格式**: `<type>(<scope>): <desc>`
- **type**: `feat` / `fix` / `chore` / `test` / `docs` / `refactor` / `build`
- **scope**: `cli` / `blender` / `flutter` / `dlss` / `core` / `ci` / `docs`
- **每个 commit 前**: build + 影响范围内 tests 全过

每个 task 的具体 commit 信息见该 task 的 `Commit` 字段。

---

## Success Criteria

### Verification Commands
```bash
# Build
cmake -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release   # Expected: 0 warnings, 0 errors

# Test
ctest --test-dir build --config Release --output-on-failure   # Expected: all pass
cd gui && flutter test && flutter test integration_test       # Expected: all pass
cd blender && python -m pytest tests/                         # Expected: all pass

# CLI smoke
build\Release\dlss-compositor-cli.exe --version  # Expected: version banner with DLSS 310.6.0
build\Release\dlss-compositor-cli.exe --self-check  # Expected: NGX SR + FG available

# End-to-end (sample fixture)
build\Release\dlss-compositor-cli.exe --config tests/fixtures/dlssconfig.json --output out/
ls out/*.exr | wc -l   # Expected: 2*N-1 frames for FG 2x
```

### Final Checklist
- [ ] All "Must Have"(31 items)present
- [ ] All "Must NOT Have"(20 items)absent(grep verified)
- [ ] DLSS submodule = `v310.6.0` tag
- [ ] All 42 implementation tasks(T25 gap)committed
- [ ] All 4 Final Wave reviewers APPROVE
- [ ] User explicit "okay"
- [ ] Draft `.omo/drafts/dlss-compositor-v2.md` deleted
