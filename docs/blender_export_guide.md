# Blender Export Guide — DLSS Compositor v2

This guide explains how to configure Blender for DLSS Compositor v2 rendering and generate the `dlssconfig.json` that drives the engine.

---

## 1. Install the Extension

1. Open Blender 5.1+
2. Go to **Edit → Preferences → Extensions**
3. Click the dropdown ▼ in the top-right corner
4. Select **Install from Disk**
5. Navigate to the repo and select the `blender/dlss_compositor/` folder (or a `.zip` of it)
6. Enable **DLSS Compositor** in the Extensions list

**Verify**: Open **Render Properties** — you should see the **DLSS Compositor** panel.

---

## 2. Set Output Resolution

Set your target output resolution in **Blender's standard Render Properties**:
- **Render Properties → Format → Resolution X / Y**
- This is your **output** (post-DLSS) resolution

⚠️ **Do NOT set this in the DLSS Compositor panel** — the plugin reads directly from Blender's render settings.

---

## 3. Choose Quality Mode

In **Render Properties → DLSS Compositor**:

1. **Enable the panel** by clicking the checkbox in the panel header (`☐ DLSS Compositor`)
2. Select a **Quality Mode** from the horizontal toggle row:

| Mode | Upscale Ratio | Render Resolution (of output) | Jitter Phases |
|------|--------------|-------------------------------|---------------|
| **DLAA** | 1× (no upscale) | 100% | 8 |
| **Ultra Quality** | 1× | 100% | 8 |
| **Quality** | 1.5× | 67% | 18 |
| **Balanced** | 1.724× | 58% | 24 |
| **Performance** | 2× | 50% | 32 |
| **Ultra Performance** | 3× | 33% | 72 |

> **Example**: 4K output at Balanced quality → Blender renders at 2227×1253 (58% of 3840×2160)

3. Set **Output Directory** — where EXR frames + `dlssconfig.json` will be written

---

## 4. Apply Settings

Click the **Apply Settings** button. This configures:

- ✅ **View Layer passes**: Enables Combined, Depth (Z), and Vector passes
- ✅ **Cycles denoising**: Forces `scene.cycles.use_denoising = True`
- ✅ **Compositor nodes**: Creates/updates File Output nodes for Combined, Depth, and Vector EXRs
- ✅ **OCIO override**: Forces `color_management = OVERRIDE` + `view_transform = Raw` on all File Output nodes (ensures scene-linear EXR output)

> **Note**: Apply Settings does **not** change `resolution_percentage`. That is managed transparently by the render handler at render time.

---

## 5. Render the Animation

Press **Ctrl+F12** (or Render → Render Animation).

**During render (transparent):**
- The handler automatically overrides `resolution_percentage` to the DLSS-appropriate value
- Each frame generates a `frames/<N>.cam.json` with camera matrices + jitter

**After render completes:**
- All per-frame `.cam.json` files are merged into `dlssconfig.json`
- `resolution_percentage` is silently restored to your original value

---

## 6. Open in DLSS Compositor GUI

1. Launch `dlss_compositor_gui.exe`
2. Click **Open dlssconfig.json** → navigate to your output directory
3. The GUI reads the config and displays output resolution, quality, FPS, and frame count
4. Configure SR preset, FG multiplier, output format, then click **Start**

---

## Gotchas

### ⚠️ Gotcha 1: Quality + Resolution Are Locked Together
Changing Quality Mode after rendering **invalidates the render**. The `resolution_percentage`, jitter sequence, and phase count are all determined by the quality selection. **Changing quality mid-render or after rendering requires a full re-render.**

### ⚠️ Gotcha 2: Cycles Denoising Must Be On
DLSS is trained on denoised frames. Disabling Cycles denoising produces noisy SR output. **Apply Settings forces denoising on**. Do not disable it manually after applying.

### ⚠️ Gotcha 3: OCIO Must Stay at Raw
The File Output nodes are configured with `color_management = OVERRIDE` and `view_transform = Raw` to output scene-linear EXR. If you change the view transform, **click Apply Settings again** to restore Raw mode.

### ⚠️ Gotcha 4: HDR Projects Use Rec.2100-PQ Display Device
For HDR rendering, set **Render Properties → Color Management → Display Device** to `Rec.2100-PQ` or `Rec.2100-HLG`. The plugin derives `is_hdr` automatically — this is **not user-configurable**.

### ⚠️ Gotcha 5: Do Not Export Camera Data Manually
v2 exports camera data **silently** during render (no separate button needed). The v1 "Export Camera Data" button does not exist in v2. Camera matrices are automatically collected per-frame.

### ⚠️ Gotcha 6: No cut_after Field
Camera cuts are detected by the CLI engine via matrix discontinuity analysis. The `dlssconfig.json` does **not** contain a `cut_after` field. Do not add one manually.

---

## Troubleshooting

**"dlssconfig.json not found after render"**
→ Check that `enabled` checkbox is ticked in the panel header
→ Check that Output Directory is set and writable

**"EXR output has wrong colors"**
→ Click Apply Settings to re-apply OCIO Raw override

**"Jitter sequence looks wrong"**
→ Ensure you selected the quality mode BEFORE rendering (changing after = re-render)
