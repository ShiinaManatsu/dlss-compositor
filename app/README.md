# app — pipeline-v3 Electron 壳

Spectrum 2 深色壳，接到 Stage 2/3。不走 Flutter，不走 in-process plugin。

冷启动进最近 `.dcp` 第一屏（`CardView` / 空状态 `IllustratedMessage`）。工程是 blend 旁的 JSON `.dcp`：模式、覆盖、导出、文件名模板、分隔条。自定义标题栏（无系统边框）。覆盖项默认不打勾；打勾才把该字段写进 inject。非 Cycles 时 RR 禁用。RR 的 Denoise 行只读（RR 自己降噪，已关 Cycles Denoise）。

导出按钮会：

1. `ramdisk.discover()` 现有 `DLSSRAM`（默认 `R:`），**不建盘、不卸盘**
2. spawn `dlss-compositor-cli --rpc-stdio`
3. RPC `start_job` / 轮询 `progress` / `preview_frame`
4. spawn `blender -b --python pipeline/inject.py`

ImDisk 未装时高级面板写 `请安装 ImDisk Toolkit`，不会假装已装。NR 禁用。

```powershell
cd app
npm install
npm run dev
```

生产：

```powershell
npm run build
npx electron .
```
