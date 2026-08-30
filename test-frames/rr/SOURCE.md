# RR test source

Marker file. This directory is the **Cycles / DLSS-RR** integration-test source.

| | |
|---|---|
| Blend | `F:\Projects\Blender\Films\TypeAsuka\SnowMix\5.render_material_remaster.blend` |
| Engine | Cycles |
| Frames | 300–350 |
| Sampling | `TABULATED_SOBOL` (Classic / white-noise-like, DLSS-RR §3.5) |
| Denoising | Off (noisy beauty; guides via `denoising_store_passes`) |
| EXR | multilayer ZIP, 10 channel groups (Image + Z + Vector + 7 denoising guides) |
| Preview JPEG | `preview/` at quality 5 (not a pipeline input) |

SR test frames will live in `test-frames/sr/` and come from EEVEE, not this directory.

EXR/JPEG binaries are gitignored. Regenerate with:

```bat
python tools/export_test_frames.py --mode rr --output-dir test-frames/rr --frame-start 300 --frame-end 350
```
