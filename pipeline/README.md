# pipeline-v3 Stage 2 + Stage 3 + Stage 4

RAM disk + Blender CLI inject + long-lived `dlss-compositor-cli` consume/delete.
No Blender addon install, no `wm.save`.

## Layout

- `inject.py` — session-only compositor inject (File Output + frame-number jitter). Writes a blend field only when the matching CLI flag is present.
- `inspect_blend.py` / `inspect_scene.py` — `blender -b --python inspect_blend.py` scene snapshot for the inspector. Never saves, never uses a member GUI instance. Not named `inspect.py` (that shadows stdlib `inspect` when `pipeline/` is on sys.path).
- `jitter.py` — Halton(2,3) copied from the v2 addon so the addon does not need to be enabled
- `ramdisk.py` — ImDisk create / unmount
- `run.py` — Stage 3 default: discover existing DLSSRAM, spawn CLI JSON-RPC, consume/delete, never unmount. Default is **one Blender pass** at the quality/blend percentage. `--native-res-buffers` opts into native-res buffers: SR with percentage < 100 does serial Beauty (render_res) + Guides (output_res, 1 spp Vector+Z) per RAM-disk window; already 100% skips the empty second pass; RR is one output-res pass. `--stage 2` still creates and unmounts.
- `rpc_client.py` — LSP `Content-Length` JSON-RPC client for `--rpc-stdio`

Experiments that used to live under `experiments/` are parked at
`F:\Projects\GitRepos\dlss-compositor-experiments` (reference only).

## ImDisk

Recommended, not a silent fallback. If `imdisk.exe` is missing, `run.py` exits 2 and prints:

```text
请安装 ImDisk Toolkit
```

It will not pretend a RAM disk exists, and it will not dump EXRs onto the working disk.

Default volume: **16 GB NTFS**, first free letter from `R:` onward. Create *and* unmount failures both attempt unload.

Release installer (not Stage 2) should ship / install ImDisk Toolkit.

```powershell
python pipeline/ramdisk.py status
```

## Run

Stage 3 uses the **already mounted** ImDisk (volume label `DLSSRAM`, default `R:`).
It will not call `ramdisk.py create` or `unmount`. Release GUI/CLI later needs
an elevated token for `imdisk -p` NTFS format; this stage does not request UAC.

```powershell
python pipeline/ramdisk.py discover --letter R
```

```powershell
# Stage 3: attach R: → one Blender pass at quality/blend percentage → consume/delete → MOV
python pipeline/run.py `
  --mode sr `
  --blend <scene.blend> `
  --frame-start 1 --frame-end 6 `
  --quality BALANCED `
  --export-dir build/stage3-out `
  --output-format prores_422hq
# Default: one pass, no native-res dual stitch.
# --native-res-buffers --percentage 58 --ram-window-frames 3  → 每窗 Beauty+Guides
# --native-res-buffers --percentage 100  → 一趟，禁止空转第二趟
# --mode rr --native-res-buffers  → 一趟输出分辨率
# output_template defaults to {job}_{mode}_v{version} resolved from the blend stem.
```

```powershell
# SR + Frame Generation 2x (output frames = 2N-1).
python pipeline/run.py `
  --mode sr --fg 2x `
  --blend <scene.blend> `
  --percentage 58 `
  --frame-start 1 --frame-end 3 `
  --export-dir build/stage3-fg-out `
  --output-format png_16
```

```powershell
# RR + Frame Generation 2x (same formula; RrFgPipeline). Cycles only.
python pipeline/run.py `
  --mode rr --fg 2x `
  --blend <scene.blend> `
  --frame-start 1 --frame-end 4 `
  --export-dir build/stage3-rr-fg-out `
  --output-format png_16
```

```powershell
# Agent no-UI job (CLI already running against the RAM disk)
.\build\x64-windows-msvc-release\Release\dlss-compositor-cli.exe `
  --job --mode rr --input-dir R:\dlss --output-dir build\stage3-out `
  --output-format prores_422hq --output-template shot_rr_v1.mov `
  --frame-start 1 --frame-end 3 --window 6
```

```powershell
# Agent no-UI SR+FG job against existing RAM-disk EXRs (does not unmount R:)
.\build\x64-windows-msvc-release\Release\dlss-compositor-cli.exe `
  --job --mode sr --fg 2x --input-dir R:\dlss --output-dir build\stage3-fg-out `
  --output-format png_16 --output-template shot_sr_v1_{frame:04d}.png `
  --frame-start 1 --frame-end 3 --window 6
```

```powershell
# Agent no-UI RR+FG job (does not unmount R:)
.\build\x64-windows-msvc-release\Release\dlss-compositor-cli.exe `
  --job --mode rr --fg 2x --input-dir R:\dlss --output-dir build\stage3-rr-fg-out `
  --output-format png_16 --output-template shot_rr_v1_{frame:04d}.png `
  --frame-start 1 --frame-end 4 --window 6
```

```powershell
# Stage 2 leftover: create + unmount (do not use while the member disk is mounted)
python pipeline/run.py --stage 2 --blend <scene.blend> --frame-start 1 --frame-end 3
```

```powershell
# inspect scene (never saves; do not attach to a member-owned Blender)
& "$env:BLENDER_EXE" -b <scene.blend> --python pipeline/inspect_blend.py -- --json-out build/stage4-out/inspect.json
```

```powershell
# inject only (still never saves). Used for TypeAsuka dry-run.
# Omit a flag to leave that blend field untouched. `--samples 32` is an explicit pin.
& "$env:BLENDER_EXE" -b <scene.blend> --python pipeline/inject.py -- `
  --output-dir R:\dlss --dry-run --frame-start 100 --frame-end 104 --samples 32
```

Do not save member `.blend` files. TypeAsuka is read-only for this stage.

## Vector socket

File Output Vector item is `VECTOR` with `vector_socket_dimensions = 4` (not 3). Combined is RGBA, Z is FLOAT, format `OPEN_EXR_MULTILAYER` + `view_transform = Raw`.

Existing compositor File Output nodes are **left alone** (not muted, not deleted, not rewired). A new File Output labeled `DLSS_Pipeline_V3` writes to the RAM disk; the engine consumes only that sequence. `scene.render.filepath` is pointed at the RAM disk for the session so the default Combined still does not land on the working disk. The blend is not saved, so none of this persists.

## CLI communication

Stage 2: start, one JSON object per stderr line, Ctrl+C kills Blender's process tree.

Stage 3: long-lived `dlss-compositor-cli --rpc-stdio` (LSP `Content-Length` frames).
Methods: `start_job` / `stop` / `progress` / `preview_frame`. `--job` is the no-UI
agent command. `--mode nr` is reserved (`DLSS5-NR 模型未到，接口已预留`).
`--fg 2x..6x` (or RPC `fg` / `fg_multiplier`) selects SR+FG (`SrFgPipeline` /
same-res `FgOnlyPipeline`) or RR+FG (`RrFgPipeline`); output frame count
is `N * fg - (fg - 1)`. NGX / driver create failure is `RR+FG 创建失败`.

Stop kills Blender + the job and **does not unmount** the RAM disk. Final MOV/sequence
is written to `--export-dir` on the working disk.

RR (`--mode rr`) forces `--engine CYCLES`, turns on `denoising_store_passes`, and sets
`use_denoising = False` (RR is the denoiser). That is not a user pin: `--use-denoising`
is ignored in RR. SR leaves Cycles denoise / sampling alone unless the matching flag is
passed. `--mode nr` still exits with `DLSS5-NR 模型未到，接口已预留`. Default is one
Blender pass at the blend/quality percentage. `--native-res-buffers` is opt-in
(`start_job.native_res_buffers`). Render percentage must stay in the NGX floor
for that quality (1080p Balanced min 960×540; 25% is rejected). Two different
buffer sizes use two Blender passes — do not stuff two resolutions into one
File Output.

## Stage 4 app

Electron + Spectrum 2 in `app/`. It discovers the existing DLSSRAM volume, never
creates or unmounts, and the 导出 button talks JSON-RPC to `dlss-compositor-cli`.
Inspector 导出组 **帧生成** picker (关 / 2x..6x, default 关) writes `.dcp` `fg`
and `start_job.fg` (`2x`..`6x`; omit when off). SR and RR can both pick FG.
EEVEE+FG uses the existing SR path; EEVEE cannot select RR.

```powershell
cd app
npm install
npm run build
npx electron . -- --blend <scene.blend> --mode sr --frame-start 84 --frame-end 93 --percentage 58 --export-dir ..\build\stage4-out --auto-export
```
