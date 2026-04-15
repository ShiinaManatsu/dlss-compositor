# DLSS Input Specification

This document details the technical requirements for the EXR buffers passed to DLSS-SR.

## Buffer Requirements

DLSS Compositor extracts the following buffers from the input MultiLayer EXR sequence.

| Buffer | Blender Pass | Channels | Format | Description |
|--------|-------------|----------|--------|-------------|
| **Color (noisy)** | `Combined` | RGBA | `R16G16B16A16_SFLOAT` | HDR linear light render. |
| **Depth** | `Depth` (Z) | 1ch | `R32_SFLOAT` | Linear world-space distance from camera. |
| **Motion Vectors** | `Vector` | 2ch (XY) | `R16G16_SFLOAT` | Pixel-space movement (Current to Previous). |
| **Diffuse Albedo** | `DiffCol` | RGB | `R16G16B16A16_SFLOAT` | (Optional) Surface diffuse color (no lighting). |
| **Normals** | `Normal` | XYZ | `R16G16B16A16_SFLOAT` | (Optional) World-space normals in range `[-1, 1]`. |
| **Roughness** | `Roughness` | 1ch | `R32_SFLOAT` | (Optional) Surface roughness. |

### Note on Blender 4.2+
When using the DLSS Compositor Blender Extension, these passes are configured automatically. Ensure you use the "Configure All Passes" button in the Render Properties panel to maintain compatibility with this specification.

### Optional Buffers
If the optional buffers (Albedo, Normals, Roughness) are missing from the EXR, the tool will substitute them with default values:
- **Diffuse Albedo**: Flat White (1.0, 1.0, 1.0)
- **Normals**: Facing camera (+Z world space)
- **Roughness**: 0.5

DLSS-SR uses these G-buffer hints to improve edge reconstruction and temporal stability. While the tool can function without them (using the defaults), providing them will result in significantly better image quality, especially in complex scenes.

## Motion Vector Conversion

Blender's `Vector` pass outputs 4 channels:
- `R`, `G`: Pixel speed from the previous frame to the current frame.
- `B`, `A`: Pixel speed from the current frame to the next frame.

DLSS-SR expects **Current to Previous** motion vectors.
1. We take the `R` and `G` channels.
2. We negate them to flip the direction from "Prev -> Curr" to "Curr -> Prev".
3. We negate the `Y` component to account for the coordinate system difference.
4. We scale the vectors by `1/width` and `1/height`.

**Formula:**
```
mv_dlss.x = -mv_blender.r / width
mv_dlss.y = mv_blender.g / height  // Double negative: -(-mv.y)
```

## Known Limitations

- **Jitter**: Version 1.0 does not support sub-pixel jitter offsets. Jitter is assumed to be zero.
- **Single Frame Quality**: DLSS-SR is a temporal upscaler. Evaluation of a single isolated frame will result in significantly lower quality than a sequence.
- **Translucency**: Currently, only opaque surface information is handled via the primary G-buffer passes.
