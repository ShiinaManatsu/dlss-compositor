# Vector Pass MV Channel Mapping - Empirical Sanity Result

## Status: Verified With Blender 5.1 File Output

## Verdict

```text
DLSS_MV = (Blender.Vector.X, -Blender.Vector.Y)
```

## Probe

Temporary Blender 5.1 scene:

- Orthographic 64x64 render.
- One plane moves right by 6.4 pixels from frame 1 to 2 to 3.
- Frame 2 is exported through a compositor File Output `VECTOR` socket with 4 channels.

Measured non-zero Vector channels in `mvprobe_0002.exr`:

```text
Vector.X = -6.4
Vector.Y =  0.0
Vector.Z = -6.4
Vector.W =  0.0
```

For a pixel in the current frame, DLSS expects the vector back to the previous frame. A right-moving object therefore needs a negative X vector, so Blender `Vector.X` already has the correct horizontal sign for DLSS.

DLSS uses +Y down in screen space. Blender's exported Vector Y needs one vertical sign flip in our Vulkan/DLSS path, so `Vector.Y` is negated.

`Vector.Z/W` describe the other temporal neighbor and are not used for DLSS current-to-previous motion input.
