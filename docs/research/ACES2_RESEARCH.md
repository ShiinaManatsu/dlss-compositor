# ACES 2.0 Output Transform Research (1000 nits, P3D65, ST.2084/PQ)

## Key Architectural Differences: ACES 2.0 vs ACES 1.x

### ACES 1.x
- RRT (Reference Rendering Transform) + ODT (Output Device Transform) concatenated
- Tonemapping applied directly to AP1 RGB channels
- Single peak luminance per transform preset

### ACES 2.0 (NEW)
- Unified "Output Transform" with parameterizable peak luminance (100-10000 nits)
- Intermediate JMh color space (simplified Hellwig 2022 CAM):
  - J = Lightness (perceptual brightness)
  - M = Colorfulness (saturation-like)
  - h = Hue (0-360 degrees)

## ACES 2.0 Rendering Transform Pipeline

Sequential processing in JMh space:

```
1. ACES 2065-1 RGB
    ↓
2. RGB → JMh (CAM conversion, ~Hellwig 2022)
    ↓
3. Tonescale (operates on J only)
   - Maps scene luminance to display peak
   - Invertible curve with highlight rolloff
    ↓
4. Chroma Compression (operates on M only)
   - Maintains hue, compresses colorfulness
   - HDR/SDR appearance matching
    ↓
5. Gamut Compression (operates on J & M)
   - Brings out-of-gamut colors to display primaries
   - Hue-indexed lookup table (360 samples)
    ↓
6. JMh → Limiting RGB (inverse CAM)
    ↓
7. White Limiting (clip to peak luminance)
    ↓
8. Display Encoding
   - RGB → XYZ
   - EOTF inverse (ST.2084/PQ for 1000-nit)
   - Target display RGB
```

## 1000-nit P3D65 ST.2084 Implementation Details

### Peak Luminance & CAM Parameters
From Nick Shaw's ACES 2.0 RC1 implementation:

```
daniele_n = 1000.0           // peak white luminance
daniele_m_2 = 10.1729113787  // M scaling factor
daniele_s_2 = 5.8959268851   // saturation parameter
daniele_u_2 = 0.9869191713   // U parameter
```

### Tonescale Parameters
```
limitJmax = 283.249899        // maximum lightness in JMh
midJ = 40.8168834391          // ~15 nits equivalent in J space
focusDist = 3.7125            // gamut compression focus distance
```

### Chroma Compression Parameters
```
sat = 0.403                   // base saturation
sat_thr = 0.0005              // saturation threshold
compr = 10.32                 // compression strength
chromaCompressScale = 2.4845958  // pow(0.03379 * 1000, 0.30596) - 0.45135
```

### Critical Math: Color Matrices

P3D65 to XYZ:
```
[0.4865709486  0.2656676932  0.1982172852]
[0.2289745641  0.6917385218  0.0792869141]
[-0.0000000000 0.0451133819  1.0439443689]
```

XYZ to P3D65:
```
[2.4934969119  -0.9313836179 -0.4027107845]
[-0.8294889696  1.7626640603  0.0236246858]
[0.0358458302  -0.0761723893  0.9568845240]
```

### ST.2084 (PQ) EOTF Inverse
For display encoding:

```
// Precise definition
m1 = 2610 / (4096 * 4)       = 0.1593017578125
m2 = 2523 / (4096 * 128)     = 0.0068408203125
c1 = 3424 / 4096             = 0.8359375
c2 = 2413 / 4096             = 0.58984375
c3 = 2392 / 4096             = 0.58447265625

V_p = pow(L, m1)
V_inv = (c1 + c2 * V_p) / (1 + c3 * V_p)
N = pow(V_inv, m2)
```

### Gamut Compression
Uses 360-element lookup table indexed by hue:

```
gamutCuspTable[360] = {
  {J, M, h},  // Per-hue J, M cusp values
  ...
}

gamutCuspTableReach[360]  // Display gamut boundary per hue
gamutTopGamma[360]        // Gamma correction per hue
```

## Implementation Sources (with GitHub permalinks)

### Official ACES 2.0 Reference
- **ACES Core**: https://github.com/aces-aswf/aces-core
- **ACES Output Transforms**: https://github.com/aces-aswf/aces-output
- **Documentation**: https://docs.acescentral.com/system-components/output-transforms/

### Real-World P3D65 1000-nit Implementation
Nick Shaw's Working Group experiments:
https://github.com/nick-shaw/aces-ot-vwg-experiments/blob/master/dctl/ACES%20Transforms/IDT/ACES2OT_rc1/ACES2OT_rc1_PQ_P3D65_1000nit_inverse.dctl

Contains:
- Full DCTL (CUDA-like) implementation
- Forward and inverse transforms
- All required constants and lookup tables
- ~3000 lines including LUT data

### OpenColorIO Integration
**Latest Versions:**
- OCIO 2.4.0+ (Preview ACES 2.0 support)
- OCIO 2.4.2+ (Production-ready ACES 2.0)
- OCIO 2.5.0+ (Vulkan support, built-in ACES 2 configs)

**GPU Shader Generation:**
- GLSL (OpenGL)
- HLSL (DirectX 11/12)
- MSL (Metal)
- Generated from CTL → XML → GPU code

**Performance Characteristics (OpenColorIO 2.4.2):**
- CPU: 4-5x slower than ACES 1 (complex math, LUTs)
- GPU: 20-35% slower than ACES 1 after 2.4.2 optimization
- Main bottleneck: 360-element gamut cusp LUT

## Recommended Vulkan Implementation Strategy

### Option 1: Compute Shader with LUT Buffers
- Store gamutCuspTable, gamutCuspTableReach, gamutTopGamma as storage buffers
- Hue-indexed lookup: `lut_value = gamutCuspTable[int(hue * 360 / 6.28)]`
- Full algorithm in shader, no baking
- Complexity: High, performance: ~20-30ms for 4K

### Option 2: Hybrid CPU-GPU
- Compute tonescale + chroma on GPU (simpler)
- Gamut compression on CPU or as pre-baked LUT3D
- Trade-off: Speed vs accuracy

### Option 3: Pre-baked 3D LUT (Fastest)
- Render ACES RGB → Display RGB lookup table offline
- Store as 64³ or 32³ 3D texture
- Runtime: Single 3D texture lookup + tetrahedral interpolation
- Trade-off: Accuracy vs speed (quantization error ~0.1-1%)

## For Inverse Transform

ACES 2.0 inverse transforms are provided by ACES (aces-aswf/aces-output).

Key inverses:
```
Display RGB → ACES 2065-1 (inverse EOTF + color matrix)
ACES 2065-1 → Display RGB (forward transform)
```

Inverse operations:
- Tonescale: Invertible (Newton-Raphson or closed-form)
- Chroma compression: Invertible (reverse toe function)
- Gamut compression: Invertible (line-based compression)
- JMh: Invertible CAM conversion

Nick Shaw's implementation includes both forward and inverse.

## Blender/OpenColorIO Compatibility Notes

Blender uses OCIO for color management. To match Blender's ACES 2.0:

1. Use OCIO 2.4.2+ with ACES 2 configs
2. Apply same parameter values for target luminance (1000 nits)
3. Use same color matrices (P3D65)
4. Use same EOTF encoding (ST.2084)

Blender's OCIO will use generated GPU shaders from CTL transforms, so direct matching is achievable.

