# DLSS Compositor

[简体中文](README.zh-CN.md)

DLSS Compositor brings NVIDIA DLSS to offline Blender rendering on Windows. Use Super Resolution (SR) to upscale, Ray Reconstruction (RR) with Cycles render passes, and Frame Generation (FG) to interpolate frames. The separate NR / FG tool processes existing video and PNG sequences.

## Requirements

- 64-bit Windows 10 or 11.
- An NVIDIA RTX GPU and a Vulkan-capable NVIDIA driver. Available DLSS features and FG multipliers depend on the GPU and driver.
- Blender 5.1 or later for SR exports. Use Blender 5.3 or later for the full Cycles RR guide passes.
- Optional ImDisk Toolkit for RAM-disk caching. A disk-backed cache folder works without ImDisk.

## Export from Blender

Check [GitHub Releases](https://github.com/ShiinaManatsu/dlss-compositor/releases) for available Windows builds, or build the desktop app from source.

1. In **项目** (Projects), click **新建** (New) and select a `.blend` file. The app creates a `.dcp` project and reads the scene. Use **打开** (Open) to resume an existing `.dcp`.
2. In **导出** (Export), choose SR or RR, a quality preset, the frame range, output resolution, format, and destination. For native-resolution SR, choose DLAA and render at 100%. For frame interpolation, select a supported **帧生成** (Frame Generation) multiplier from 2× to 6×.
3. Set a **缓存路径** (cache folder) or enable **使用内存盘** (Use RAM disk) in an elevated session with ImDisk. Click **导出** (Export) in the title bar. Find the results in **Jobs**.

The app launches Blender in the background and configures render passes and camera jitter for the session. Your source `.blend` stays intact. The desktop workflow needs no Blender extension.

| Blender export | Desktop output formats |
| --- | --- |
| SDR | ProRes 422 HQ (MOV), scene-linear OpenEXR, 16-bit PNG |
| HDR | ProRes 422 HQ (MOV, HDR10/PQ), scene-linear OpenEXR |

The cache holds temporary multilayer EXRs, which the engine reclaims as it processes them. **滚动并行渲染** (Rolling parallel rendering) lets multiple Blender processes render frame ranges while a single DLSS session processes frames in order. Bake stateful simulations before using multiple Blender processes.

## NR / FG tool

Open **NR / FG** and select an SDR video or the first frame of a numbered PNG sequence. Preview the result, adjust NR and FG, and export to ProRes 422 HQ (MOV), H.265 (MP4), or a 16-bit PNG sequence.

NR can run without guides. For guided NR or FG, export SDR ProRes or PNG from Blender with **导出 NR 引导视频（MKV）** (Export NR guide video) enabled. This turns off FG for that export so the color output and guides stay aligned. Load the color output and its `*_dlssbuffer` folder in the NR / FG tool. The folder holds `dlssnrbuffer.mkv` and `dlssconfig.json`. The tool also accepts legacy EXR guides. FG requires both guide buffers and the camera data in `dlssconfig.json`.

NR requires a compatible `nvngx_dlssnr.dll` alongside the CLI. The local NR installer described below embeds that DLL.

## Build from source

Use an x64 Visual Studio Developer PowerShell with the C++ desktop workload, CMake 3.25+, Ninja, Git LFS, and Node.js 22.12+ with npm. Set `VCPKG_ROOT` to a bootstrapped vcpkg checkout. Shader compilation uses the Vulkan SDK's `glslc` or vcpkg's `glslang[tools]`.

```powershell
git clone https://github.com/ShiinaManatsu/dlss-compositor.git
Set-Location dlss-compositor
git submodule update --init --recursive
git lfs pull

$env:VCPKG_ROOT = 'C:\vcpkg'  # Use your vcpkg path.
cmake --preset x64-windows-msvc-release
cmake --build --preset x64-windows-msvc-release --parallel

Set-Location app
npm.cmd ci
npm.cmd run dev
```

If the app cannot find Blender, set `BLENDER_EXE` to the full path of `blender.exe` before launching it.

- `npm.cmd run dist` in `app/` builds the standard Windows installer and ZIP. The installer and ZIP exclude the NR DLL.
- `npm.cmd run dist:local-nr` in `app/` builds a single `DLSS-Compositor-Setup-<version>-NR.exe` for local use. Place your compatible `nvngx_dlssnr.dll` in the repository's `resources/` folder before building.

Both commands write their artifacts directly to `build/release/`.

## Repository

- Electron desktop app and packaging in [`app/`](app/).
- C++ Vulkan/NGX engine and CLI in [`cli/`](cli/). Run `dlss-compositor-cli.exe --help` for batch and JSON-RPC options.
- Blender scene inspection and background rendering scripts in [`pipeline/`](pipeline/).
- Blender extension for manual pass and camera-data export in [`blender-plugin/`](blender-plugin/).

## License

MIT. See [LICENSE](LICENSE).
