# Blender 5.3 / DLSS verification — 2026-11-11

Scene: `pool1S.blend`, frames 50–53 of 50–3936, 1920×1080 at 30 fps, Cycles on RTX 5070 Ti (OptiX, driver 621.14). RR Quality rendered at 1286×723, 4 samples, motion blur off. Blender 5.3.0 release hash `02f0899d78e3`. The source blend was read-only; all profiling overrides lived in the Blender subprocess.

## Release changes that affect this project

- [Blender 5.3](https://www.blender.org/download/releases/5-3/) ships DLSS 4.5 RR for Cycles viewport denoising. The project's final-frame path still runs Cycles without its render denoiser and consumes the exported passes. The new compositor playback cache and bundle nodes are useful for interactive compositing; they do not replace File Output or the external pipeline.
- [`Scene.compositor_effects`](https://docs.blender.org/api/5.3/bpy.types.Scene.html#bpy.types.Scene.compositor_effects) is the scene effect stack. Its deprecated predecessor now returns the active effect's tree. The session injector uses an independent pass-through effect, and the inspector checks every effect for File Outputs. The extension also uses the stack on 5.3.
- `Denoising Albedo` and `Denoising Specular Albedo` were already mapped to RR's diffuse and specular albedo. `denoising_store_motion_passes` is a separate Cycles 5.3 flag. Both new motion passes were present in the EXR, but the reader stopped at the last required part before reaching them. With that fixed, reflection motion reaches RR and the main motion input still uses `Vector`. Diffuse/glossy lighting passes remain unconnected. Cycles omits the extra motion passes when motion blur is enabled.
- [Public DLSS SDK](https://github.com/NVIDIA/DLSS/releases) remains 310.9.16 with RR Preset F (already the default here). [Magpie v0.6.11](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.11-experimental) offers a separate original RTX 50 NR DLL. NVIDIA Authenticode validated the local 310.9.16.0 file (SHA-256 `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`). The old local DLL is backed up under `build/pool53-upstream/`. The NR DLL is excluded from public packaging.

## Guide storage and playback

`<output>_dlssbuffer/dlssnrbuffer.mkv` holds one FFV1 gray16 stream with three panels per frame: signed MV.x, signed MV.y, and NDC depth. The planes are data samples, with no color transform or subsampling. Motion is rounded to 1/32 px (at most 1/64 px error); the depth half bits match the old EXR. Each frame is independently coded. The tool still reads a directory of legacy EXRs, and reads the copied `dlssconfig.json` for FG camera data.

The four-frame guide video is 1,201,434 bytes vs 3,090,074 bytes for the same original guides encoded as ZIP EXRs (61% smaller). The decoded depth was bit-for-bit identical, the maximum measured motion error was exactly 1/64 px, and the merged segments decoded identically to the single export. NR B + FG H.265 against the same path using unrounded guides measured 52.7 dB PSNR on this clip. NR evaluation on warm 1080p frames took about 5.5 ms for A and 3.3 ms for B (40% lower); Auto and B produced matching previews on this clip.

The local release CLI exercised RR F → ProRes + guides, RR F → FG 2×, SR → PNG16, segment guide muxing, legacy EXR input, NR A/B/Auto preview and backward seek, NR → ProRes, NR B + FG 2× → H.265, and FG only → PNG16. The Electron production build and the Blender 5.3 extension operator also passed. `R:` served the intermediate EXRs; the initial Stage 3 run wrote to `S:`. The final artifacts and logs are under `build/pool53-outputs` and `build/pool53-*.log`.

## Scene performance

In the matching OptiX runs the warmed frames averaged 3.99 s with Persistent Data and 6.28 s without it. Path tracing including render result transfer took about 0.13–0.16 s, and the DLSS backend processed warm guide-export frames in about 0.10–0.13 s. The large cost was Cycles scene synchronization, including the neighboring times needed for motion passes.

The Ocean modifier on `立方体.028` was live at render resolution 40. Dropping it to 24 for the diagnostic brought warm frames to 0.71 s; disabling just that modifier brought them to 0.24 s and data synchronization from ~2.7–2.9 s to ~0.006 s. Check the appearance of the water before adopting 24. To preserve the original simulation resolution, try baking the [Ocean modifier cache](https://docs.blender.org/manual/uk/5.3/modeling/modifiers/physics/ocean.html#baking) and verify a short range before the full render. The original scene also logs a soft-body dependency warning and user add-on startup errors; all four external libraries resolved in the factory-startup profile.

These timings cover a short section. They do not establish performance, image quality, or final disk use across the full animation. The blend uses ACES 2.0 / Display P3, while the current SDR ProRes stream is tagged BT.709; color-managed delivery was not part of this verification.
