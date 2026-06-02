# Phase 2 SR Quality — Issues

## 2026-05-07 — Known Issues to Watch

### sequence_processor.cpp GBuffer usage
- GBuffer appears in THREE pipeline paths:
  1. SR-only path (lines ~763-806): diffuse, normals, roughness TextureHandle vars, texture updates, DlssSRFrameInput fields
  2. SRFG path first frame (lines ~1164-1213): firstDiffuse, firstNormals, firstRoughness handles + frame fields
  3. SRFG path per-frame (lines ~1303-1360): diffuse, normals, roughness TextureHandle vars in loop + srFrame fields
- Agent doing Task 2 MUST remove from ALL three paths (not just one)

### blender/dlss_compositor_aov/__init__.py vs blender/aov_export_preset.py
- __init__.py does NOT have `vl.use_pass_glossy_color = True` but aov_export_preset.py DOES (line 231)
- __init__.py has Roughness AOV + Principled BSDF injection
- __init__.py has all 6 pass slots including Normal, DiffuseColor, Roughness in pass_names list
- Both files need parallel updates, but they're slightly different

### exr_reader.cpp aliases to remove
- Line 44-45: `Normal.` → `RenderLayer.Normal.` — KEEP this alias (Normal is still used? Actually no, Normal pass is GBuffer)
  - Wait — looking again at the EXR reader: the Normal alias maps `Normal.X/Y/Z` → `RenderLayer.Normal.X/Y/Z` which feeds `channel_mapper.cpp:normalChannels`. After removing GBuffer from channel_mapper, this alias is dead. DELETE it.
- Line 50-52: `Diffuse Color.` → `RenderLayer.DiffCol.` — DELETE (GBuffer)
- Line 53-55: `Glossy Color.` → `RenderLayer.GlossCol.` — DELETE (GBuffer)
- Line 59-60: `Roughness.V` → `RenderLayer.Roughness.X` — DELETE (GBuffer)
- Line 47-48: `Vector.` → `RenderLayer.Vector.` — KEEP (motion vectors, not GBuffer)
- Line 41-43: `Image.` → `RenderLayer.Combined.` — KEEP (color, not GBuffer)
- Line 56-58: `Depth.V` → `RenderLayer.Depth.Z` — KEEP (depth, not GBuffer)

### DlssQualityMode enum mapping
- DlssQualityMode has: DLAA, MaxQuality, Balanced, Performance, UltraPerformance
- No "UltraQuality" in the enum! Plan says DLSS_Hint_Render_Preset_UltraQuality → K
  But the NGX parameter exists even though our DlssQualityMode doesn't have it
  → Auto preset mapping should still set UltraQuality preset for completeness
