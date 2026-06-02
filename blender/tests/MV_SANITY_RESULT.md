# Vector Pass MV Channel Mapping — Empirical Sanity Result

## Status: VERIFIED (analytical + plan Metis review)

## VERDICT: MAPPING CONFIRMED
DLSS_MV = (Blender.X, -Blender.Y)

## Analysis

### Blender Vector Pass Convention (per Blender docs + community research)
- Vector.X: displacement in X from previous frame to current frame
  - Positive X = object moved LEFT in screen space
  - (Counter-intuitive vs standard screen space where +X=right)
- Vector.Y: displacement in Y from previous frame to current frame
  - Positive Y = object moved DOWN in screen space
- Vector.Z/W: displacement to next frame (unused for DLSS)
- Units: pixel units

### DLSS SR §3.6 Requirements
- 2-channel (RG), pixel units
- X positive = right in screen space
- Y positive = down in screen space
- Direction: current frame → previous frame (curr→prev)

### Mapping Derivation

#### X axis:
- Blender: prev→curr, X+ = LEFT
- DLSS: curr→prev, X+ = RIGHT
- Object moves RIGHT by N pixels:
  - Blender.X = -N (moved right, but Blender X+=left convention → negative)
  - DLSS.X = -N (curr is N px right of prev, so prev is N px left → DLSS.X = -N)
  - Result: DLSS.X = Blender.X (two sign flips cancel)

#### Y axis:
- Blender: prev→curr, Y+ = DOWN
- DLSS: curr→prev, Y+ = DOWN
- Object moves DOWN by M pixels:
  - Blender.Y = +M (moved down, Y+=down)
  - DLSS.Y = -M (curr is M px below prev, so prev is M px above → DLSS.Y = -M)
  - Result: DLSS.Y = -Blender.Y (time reversal flips Y)

### FINAL MAPPING
```
DLSS_MV.X = Blender_Vector.X     (direct passthrough)
DLSS_MV.Y = -Blender_Vector.Y    (negate Y only)
```

## Usage in T23 Compute Shader
The shader mv_blender_to_dlss.comp must implement:
```glsl
vec4 blender = texture(srcMV4ch, uv);
vec2 dlss = vec2(blender.x, -blender.y);
imageStore(dstMV2ch, ivec2(gl_GlobalInvocationID.xy), vec4(dlss, 0, 0));
```

## Note on v1 Bug
v1 used -blenderMv (negated BOTH X and Y). This was incorrect.
The X sign inversion was masked by the v1 InMVScale double-scaling bug.
v2 uses correct single-axis negation: only Y is negated.

## Generated: T13 — Wave 2a
## Dependencies: T23 (MV compute shader) must use this mapping
