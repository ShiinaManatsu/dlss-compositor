# DLSS Compositor

[English](README.md)

DLSS Compositor 是一款面向 Blender 离线渲染的 Windows 桌面应用。它使用 NVIDIA DLSS 超分辨率（SR）放大画面、通过 Cycles 渲染通道进行光线重建（RR），并支持帧生成（FG）插帧。独立的 NR / FG 工具也可处理已有视频和 PNG 序列。

## 运行要求

- 64 位 Windows 10 或 11。
- NVIDIA RTX 显卡和支持 Vulkan 的 NVIDIA 驱动。可用的 DLSS 功能和 FG 倍数取决于显卡与驱动。
- SR 导出需要 Blender 5.1 或更新版本。完整的 Cycles RR 引导通道需要 Blender 5.3 或更新版本。
- 可选安装 ImDisk Toolkit，将内存盘用于缓存。也可直接使用磁盘上的缓存目录。

## 导出 Blender 项目

可到 [GitHub Releases](https://github.com/ShiinaManatsu/dlss-compositor/releases) 查看可用的 Windows 安装包，也可以用当前源码构建桌面版。

1. 在 **项目** 页点击 **新建**，选择 `.blend` 文件。应用会创建 `.dcp` 项目并读取场景。已有项目可用 **打开** 载入 `.dcp`。
2. 在 **导出** 页选择 SR 或 RR，并设置质量档、帧范围、输出分辨率、格式和目录。原生分辨率下使用 SR 时，选择 DLAA 并以 100% 渲染。需要插帧时，在 **帧生成** 中选择显卡支持的 2× 到 6× 倍数。
3. 设置 **缓存路径**，或在已安装 ImDisk 的管理员会话中启用 **使用内存盘**。点击标题栏的 **导出**，在 **Jobs** 中查看产物。

桌面版会在后台启动 Blender，并为本次渲染设置通道和相机抖动。原始 `.blend` 文件不会被修改。此流程无需安装 Blender 扩展。

| Blender 导出 | 桌面版输出格式 |
| --- | --- |
| SDR | ProRes 422 HQ（MOV）、场景线性 OpenEXR、16 位 PNG |
| HDR | ProRes 422 HQ（MOV，HDR10/PQ）、场景线性 OpenEXR |

缓存目录存放临时多层 EXR，处理完成的帧会被回收。开启 **滚动并行渲染** 后，多个 Blender 进程分别渲染一段帧，DLSS 在同一个会话里按帧序处理。使用多个进程前，请先烘焙依赖前序帧的模拟。

## NR / FG 工具

打开 **NR / FG**，选择 SDR 视频或编号 PNG 序列的首帧。可预览并调整 NR 和 FG，再导出 ProRes 422 HQ（MOV）、H.265（MP4）或 16 位 PNG 序列。

NR 可以在没有引导数据时运行。要使用带引导的 NR 或 FG，先从 Blender 项目导出 SDR ProRes 或 PNG，并启用 **导出 NR 引导视频（MKV）**。这次导出的 FG 会关闭，让色彩输出和引导数据逐帧对应。随后在 NR / FG 工具中加载色彩输出和对应的 `*_dlssbuffer` 目录。目录包含 `dlssnrbuffer.mkv` 和 `dlssconfig.json`。工具也支持旧版 EXR 引导序列。FG 同时需要引导数据和 `dlssconfig.json` 中的相机数据。

NR 需要在 CLI 同目录放置兼容的 `nvngx_dlssnr.dll`。下文的本地 NR 安装包会内置该 DLL。

## 从源码构建

使用 x64 Visual Studio Developer PowerShell。需要安装 C++ 桌面开发工作负载、CMake 3.25+、Ninja、Git LFS，以及带 npm 的 Node.js 22.12+。将 `VCPKG_ROOT` 指向已初始化的 vcpkg 目录。着色器使用 Vulkan SDK 的 `glslc` 或 vcpkg 的 `glslang[tools]` 编译。

```powershell
git clone https://github.com/ShiinaManatsu/dlss-compositor.git
Set-Location dlss-compositor
git submodule update --init --recursive
git lfs pull

$env:VCPKG_ROOT = 'C:\vcpkg'  # 改为本机的 vcpkg 路径。
cmake --preset x64-windows-msvc-release
cmake --build --preset x64-windows-msvc-release --parallel

Set-Location app
npm.cmd ci
npm.cmd run dev
```

如果应用没有找到 Blender，启动前将 `BLENDER_EXE` 设为 `blender.exe` 的完整路径。

- 在 `app/` 下执行 `npm.cmd run dist` 生成标准 Windows 安装包和 ZIP。使用 NR 需自备 DLL。
- 在 `app/` 下执行 `npm.cmd run dist:local-nr` 生成单文件本地安装包 `DLSS-Compositor-Setup-<version>-NR.exe`。构建前将兼容的 `nvngx_dlssnr.dll` 放进仓库的 `resources/` 目录。

两条命令的产物都直接写入 `build/release/`。

## 仓库结构

- Electron 桌面应用和打包脚本见 [`app/`](app/)。
- C++ Vulkan/NGX 引擎和 CLI 见 [`cli/`](cli/)。批处理和 JSON-RPC 参数可用 `dlss-compositor-cli.exe --help` 查看。
- Blender 场景读取与后台渲染脚本见 [`pipeline/`](pipeline/)。
- 用于手动导出渲染通道和相机数据的 Blender 扩展见 [`blender-plugin/`](blender-plugin/)。

## 许可证

MIT。见 [LICENSE](LICENSE)。
