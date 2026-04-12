#!/usr/bin/env python3
"""Quick ACES 2.0 roundtrip accuracy test."""

import numpy as np
import PyOpenColorIO as ocio

# Load config
config = None
for name in [
    "studio-config-v4.0.0_aces-v2.0_ocio-v2.5",
    "studio-config-v2.1.0_aces-v1.3_ocio-v2.4",
]:
    try:
        config = ocio.Config.CreateFromBuiltinConfig(name)
        print(f"Config: {name}")
        break
    except Exception:
        continue

src_cs = "ACES2065-1"

hdr_transforms = [
    ("Rec.2100-PQ - Display", "ACES 2.0 - HDR 1000 nits (Rec.2020)"),
    ("Rec.2100-PQ - Display", "ACES 2.0 - HDR 1000 nits (P3 D65)"),
]

fwd_proc = None
inv_proc = None
for display, view in hdr_transforms:
    try:
        fwd_proc = config.getProcessor(
            src_cs, display, view, ocio.TRANSFORM_DIR_FORWARD
        )
        inv_proc = config.getProcessor(
            src_cs, display, view, ocio.TRANSFORM_DIR_INVERSE
        )
        print(f"Display: {display}, View: {view}")
        break
    except Exception:
        continue

fwd_cpu = fwd_proc.getDefaultCPUProcessor()
inv_cpu = inv_proc.getDefaultCPUProcessor()

# Test roundtrip with neutral grays
print()
print("=== ACES 2.0 Roundtrip Test (neutral gray) ===")
print(f"{'Input':>12s}  {'Forward':>12s}  {'Roundtrip':>12s}  {'Error%':>12s}")
for val in [0.001, 0.01, 0.05, 0.18, 0.5, 1.0, 2.0, 5.0, 10.0, 50.0, 100.0]:
    pixel = np.array([[val, val, val, 1.0]], dtype=np.float32)
    fwd_result = pixel.copy()
    fwd_cpu.applyRGBA(fwd_result)

    rt_result = fwd_result.copy()
    inv_cpu.applyRGBA(rt_result)

    err = abs(rt_result[0, 0] - val) / max(val, 1e-6) * 100
    print(
        f"{val:12.4f}  {fwd_result[0, 0]:12.6f}  {rt_result[0, 0]:12.6f}  {err:11.4f}%"
    )

# Test roundtrip with saturated colors
print()
print("=== ACES 2.0 Roundtrip Test (saturated colors) ===")
test_colors = [
    (0.5, 0.1, 0.1, "red"),
    (0.1, 0.5, 0.1, "green"),
    (0.1, 0.1, 0.5, "blue"),
    (0.1, 0.5, 0.5, "cyan"),
    (0.5, 0.1, 0.5, "magenta"),
    (0.5, 0.5, 0.1, "yellow"),
    (1.0, 0.2, 0.1, "bright red"),
    (0.1, 1.0, 0.2, "bright green"),
    (0.2, 0.1, 1.0, "bright blue"),
    (5.0, 1.0, 0.5, "HDR warm"),
    (1.0, 5.0, 3.0, "HDR teal"),
]
for r, g, b, name in test_colors:
    pixel = np.array([[r, g, b, 1.0]], dtype=np.float32)
    fwd_result = pixel.copy()
    fwd_cpu.applyRGBA(fwd_result)

    rt_result = fwd_result.copy()
    inv_cpu.applyRGBA(rt_result)

    r_err = abs(rt_result[0, 0] - r)
    g_err = abs(rt_result[0, 1] - g)
    b_err = abs(rt_result[0, 2] - b)
    max_err = max(r_err, g_err, b_err)
    print(
        f"  {name:12s} in=({r:.2f},{g:.2f},{b:.2f})  fwd=({fwd_result[0, 0]:.4f},{fwd_result[0, 1]:.4f},{fwd_result[0, 2]:.4f})  rt=({rt_result[0, 0]:.4f},{rt_result[0, 1]:.4f},{rt_result[0, 2]:.4f})  maxErr={max_err:.6f}"
    )

# Now test what our actual LUT pipeline does:
# Forward: scene-linear -> log2 shaper -> LUT lookup -> display
# Inverse: display -> direct LUT lookup -> scene-linear
# Simulate with actual LUT files

print()
print("=== LUT File Roundtrip Test (simulating GPU trilinear) ===")

SHAPER_LOG2_MIN = -12.0
SHAPER_LOG2_MAX = 16.0
LUT_SIZE = 128

fwd_lut = np.fromfile("luts/aces2_1000nit_forward.bin", dtype=np.float32).reshape(
    LUT_SIZE, LUT_SIZE, LUT_SIZE, 4
)
inv_lut = np.fromfile("luts/aces2_1000nit_inverse.bin", dtype=np.float32).reshape(
    LUT_SIZE, LUT_SIZE, LUT_SIZE, 4
)

from scipy.interpolate import RegularGridInterpolator

coords = np.linspace(0.0, 1.0, LUT_SIZE)

fwd_interp_r = RegularGridInterpolator(
    (coords, coords, coords),
    fwd_lut[:, :, :, 0],
    method="linear",
    bounds_error=False,
    fill_value=None,
)
fwd_interp_g = RegularGridInterpolator(
    (coords, coords, coords),
    fwd_lut[:, :, :, 1],
    method="linear",
    bounds_error=False,
    fill_value=None,
)
fwd_interp_b = RegularGridInterpolator(
    (coords, coords, coords),
    fwd_lut[:, :, :, 2],
    method="linear",
    bounds_error=False,
    fill_value=None,
)

inv_interp_r = RegularGridInterpolator(
    (coords, coords, coords),
    inv_lut[:, :, :, 0],
    method="linear",
    bounds_error=False,
    fill_value=None,
)
inv_interp_g = RegularGridInterpolator(
    (coords, coords, coords),
    inv_lut[:, :, :, 1],
    method="linear",
    bounds_error=False,
    fill_value=None,
)
inv_interp_b = RegularGridInterpolator(
    (coords, coords, coords),
    inv_lut[:, :, :, 2],
    method="linear",
    bounds_error=False,
    fill_value=None,
)


def linear_to_shaper(val):
    clamped = np.clip(val, 2.0**SHAPER_LOG2_MIN, 2.0**SHAPER_LOG2_MAX)
    log2_val = np.log2(clamped)
    return (log2_val - SHAPER_LOG2_MIN) / (SHAPER_LOG2_MAX - SHAPER_LOG2_MIN)


print(
    f"{'Name':>12s}  {'Input RGB':>24s}  {'FwdLUT RGB':>24s}  {'InvLUT RGB':>24s}  {'MaxErr':>10s}"
)

all_tests = [
    (0.18, 0.18, 0.18, "mid gray"),
    (1.0, 1.0, 1.0, "white"),
    (0.5, 0.1, 0.1, "red"),
    (0.1, 0.5, 0.1, "green"),
    (0.1, 0.1, 0.5, "blue"),
    (0.1, 0.5, 0.5, "cyan"),
    (1.0, 0.2, 0.1, "bright red"),
    (5.0, 1.0, 0.5, "HDR warm"),
    (1.0, 5.0, 3.0, "HDR teal"),
    (10.0, 10.0, 10.0, "HDR bright"),
]

for r, g, b, name in all_tests:
    # Forward: log2 shaper -> LUT lookup
    sr = linear_to_shaper(r)
    sg = linear_to_shaper(g)
    sb = linear_to_shaper(b)

    pt = np.array([[sr, sg, sb]])
    fr = float(fwd_interp_r(pt))
    fg = float(fwd_interp_g(pt))
    fb = float(fwd_interp_b(pt))

    # Inverse: direct [0,1] -> LUT lookup
    pt_inv = np.array([[np.clip(fr, 0, 1), np.clip(fg, 0, 1), np.clip(fb, 0, 1)]])
    ir = float(inv_interp_r(pt_inv))
    ig = float(inv_interp_g(pt_inv))
    ib = float(inv_interp_b(pt_inv))

    max_err = max(abs(ir - r), abs(ig - g), abs(ib - b))
    rel_err = max_err / max(r, g, b) * 100
    print(
        f"  {name:12s}  ({r:.2f},{g:.2f},{b:.2f})  ({fr:.4f},{fg:.4f},{fb:.4f})  ({ir:.4f},{ig:.4f},{ib:.4f})  {max_err:.4f} ({rel_err:.1f}%)"
    )
