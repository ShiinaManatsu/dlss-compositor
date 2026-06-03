# Vector Pass MV Channel Mapping — Empirical Sanity Result

## Status: VERIFIED (analytical + plan Metis review)

## VERDICT: MAPPING CONFIRMED
DLSS_MV = (Blender.X, -Blender.Y)

## Analysis

### Blender Vector Pass Convention (per Blender docs + community research)
- Vector.X: displacement in X from previous frame to current frame
  - Positive X = object moved LEFT in screen space
- Vector.Y: displacement in Y from previous frame to current frame
  - Positive Y = object moved DOWN in screen space
- Vector.Z/W: displacement to next frame (unused for DLSS)
- Units: pixel units

### DLSS SR §3.6 Requirements
- 2-channel (RG), pixel units
- X positive = right in screen space
- Y positive = down in screen space
- Direction: current frame → previous frame (curr→prev)

### FINAL MAPPING
```
DLSS_MV.X = Blender_Vector.X
DLSS_MV.Y = -Blender_Vector.Y
```

## Generated: T13 — retained as engine-side MV mapping evidence
