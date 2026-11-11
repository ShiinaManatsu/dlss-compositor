# RR render sample source

Marker file for the retained **Cycles / DLSS-RR** render samples.

| | |
|---|---|
| Blend | `F:\Projects\Blender\Films\TypeAsuka\SnowMix\5.render_material_remaster.blend` |
| Engine | Cycles |
| Frames | 300–350 |
| Sampling | `TABULATED_SOBOL` (Classic / white-noise-like, DLSS-RR §3.5) |
| Denoising | Off (noisy beauty; guides via `denoising_store_passes`) |
| EXR | multilayer ZIP, 10 channel groups (Image + Z + Vector + 7 denoising guides) |
| Preview JPEG | `preview/` at quality 5 (not a pipeline input) |

SR render samples use EEVEE and are separate from this directory.

EXR/JPEG binaries are gitignored. To regenerate them, open the listed scene in
the application and use the normal RR export flow for frames 300–350.
