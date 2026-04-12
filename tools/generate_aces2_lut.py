#!/usr/bin/env python3
"""Generate ACES 2.0 Output Transform 3D LUT binaries for dlss-compositor.

Requires: PyOpenColorIO >= 2.4.2 (ships ACES 2.0 built-in config)
          NumPy

Usage:
    python generate_aces2_lut.py [--size 128] [--output-dir luts/]

Outputs:
    aces2_1000nit_forward.bin   -- scene-linear AP0 -> Rec.2100 PQ 1000-nit display
    aces2_1000nit_inverse.bin   -- Rec.2100 PQ 1000-nit display -> scene-linear AP0
    aces2_sdr_forward.bin       -- scene-linear AP0 -> sRGB 100-nit display
    aces2_sdr_inverse.bin       -- sRGB 100-nit display -> scene-linear AP0

Binary format:
    Raw float32, RGBA interleaved, size^3 * 4 * 4 bytes.
    Lattice order: R fastest, then G, then B (standard 3D LUT).
    Shaper: log2, domain [2^-12, 2^16] = [0.000244140625, 65536.0].
"""

import argparse
import os
import struct
import sys

import numpy as np

try:
    import PyOpenColorIO as ocio
except ImportError:
    print(
        "ERROR: PyOpenColorIO is required. Install via: pip install opencolorio",
        file=sys.stderr,
    )
    sys.exit(1)


SHAPER_LOG2_MIN = -12.0  # 2^-12 = 0.000244140625
SHAPER_LOG2_MAX = 16.0  # 2^16  = 65536.0


def build_shaper_lut(size):
    """Build a 1D shaper that maps linear [2^min, 2^max] to normalized [0, 1].

    Returns (to_shaper, from_shaper) as numpy arrays for verification.
    """
    # Normalized [0, 1] -> log2 -> linear
    t = np.linspace(0.0, 1.0, size, dtype=np.float64)
    log2_values = SHAPER_LOG2_MIN + t * (SHAPER_LOG2_MAX - SHAPER_LOG2_MIN)
    linear_values = np.power(2.0, log2_values)
    return linear_values


def linear_to_shaper(linear_val):
    """Map scene-linear value to [0, 1] shaper domain via log2."""
    clamped = np.clip(linear_val, 2.0**SHAPER_LOG2_MIN, 2.0**SHAPER_LOG2_MAX)
    log2_val = np.log2(clamped)
    return (log2_val - SHAPER_LOG2_MIN) / (SHAPER_LOG2_MAX - SHAPER_LOG2_MIN)


def shaper_to_linear(shaper_val):
    """Map [0, 1] shaper domain back to scene-linear via exp2."""
    log2_val = SHAPER_LOG2_MIN + shaper_val * (SHAPER_LOG2_MAX - SHAPER_LOG2_MIN)
    return np.power(2.0, log2_val)


def get_ocio_processor(
    config, src_colorspace, dst_colorspace, direction=ocio.TRANSFORM_DIR_FORWARD
):
    """Get an OCIO processor for a colorspace conversion."""
    return config.getProcessor(src_colorspace, dst_colorspace)


def generate_3d_lut(processor, lut_size):
    """Generate a 3D LUT by evaluating an OCIO processor on a shaper lattice.

    The lattice is in log2-shaper space: each axis goes [0, 1] which maps
    to scene-linear [2^-12, 2^16] via the shaper.

    Used for FORWARD LUTs (scene-linear input -> display-referred output).

    Returns: numpy array of shape (size^3, 4) as float32.
    """
    cpu = processor.getDefaultCPUProcessor()

    total = lut_size**3
    # Build lattice in R-fastest order: for each (b, g, r)
    r_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)
    g_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)
    b_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)

    # Create meshgrid: R fastest, then G, then B
    rr, gg, bb = np.meshgrid(r_coords, g_coords, b_coords, indexing="ij")
    # Flatten in Fortran order to get B slowest, G middle, R fastest
    # Actually meshgrid with indexing='ij' gives (r, g, b) axes
    # We need lattice order: iterate B outermost, G middle, R innermost
    # So reshape to (lut_size, lut_size, lut_size) with axes = (R, G, B)
    # and flatten in order B, G, R -> use transpose
    rr_flat = rr.transpose(2, 1, 0).flatten()
    gg_flat = gg.transpose(2, 1, 0).flatten()
    bb_flat = bb.transpose(2, 1, 0).flatten()

    # Convert from shaper [0,1] to scene-linear
    r_linear = shaper_to_linear(rr_flat).astype(np.float32)
    g_linear = shaper_to_linear(gg_flat).astype(np.float32)
    b_linear = shaper_to_linear(bb_flat).astype(np.float32)

    # Pack as RGBA (alpha = 1.0)
    pixels = np.zeros((total, 4), dtype=np.float32)
    pixels[:, 0] = r_linear
    pixels[:, 1] = g_linear
    pixels[:, 2] = b_linear
    pixels[:, 3] = 1.0

    # Apply OCIO transform
    result = pixels.copy()
    # Process in chunks to avoid memory issues
    chunk_size = 65536
    for start in range(0, total, chunk_size):
        end = min(start + chunk_size, total)
        chunk = result[start:end].copy()
        cpu.applyRGBA(chunk)
        result[start:end] = chunk

    return result


def generate_3d_lut_display_domain(processor, lut_size):
    """Generate a 3D LUT by evaluating an OCIO processor on a display-domain lattice.

    The lattice is a uniform [0, 1]^3 grid in display-referred space (no shaper).
    The OCIO processor (inverse transform) maps display-referred -> scene-linear.

    Used for INVERSE LUTs (display-referred input -> scene-linear output).
    The GPU shader samples this LUT directly with clamped RGB coords (no log2 shaper).

    Returns: numpy array of shape (size^3, 4) as float32.
    """
    cpu = processor.getDefaultCPUProcessor()

    total = lut_size**3
    r_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)
    g_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)
    b_coords = np.linspace(0.0, 1.0, lut_size, dtype=np.float32)

    rr, gg, bb = np.meshgrid(r_coords, g_coords, b_coords, indexing="ij")
    rr_flat = rr.transpose(2, 1, 0).flatten()
    gg_flat = gg.transpose(2, 1, 0).flatten()
    bb_flat = bb.transpose(2, 1, 0).flatten()

    # Display-domain: use lattice values directly (already [0, 1])
    pixels = np.zeros((total, 4), dtype=np.float32)
    pixels[:, 0] = rr_flat
    pixels[:, 1] = gg_flat
    pixels[:, 2] = bb_flat
    pixels[:, 3] = 1.0

    # Apply OCIO inverse transform: display-referred -> scene-linear
    result = pixels.copy()
    chunk_size = 65536
    for start in range(0, total, chunk_size):
        end = min(start + chunk_size, total)
        chunk = result[start:end].copy()
        cpu.applyRGBA(chunk)
        result[start:end] = chunk

    return result


def write_binary_lut(filepath, lut_data):
    """Write a 3D LUT as raw float32 binary."""
    lut_data.astype(np.float32).tofile(filepath)
    size_bytes = os.path.getsize(filepath)
    print(f"  Written: {filepath} ({size_bytes / 1024 / 1024:.1f} MB)")


def find_aces2_colorspaces(config):
    """Find ACES 2.0 colorspace names in the config."""
    all_cs = [cs.getName() for cs in config.getColorSpaces()]

    # Debug: print all colorspaces
    print("Available colorspaces:")
    for name in sorted(all_cs):
        print(f"  {name}")

    print()
    print("Available displays and views:")
    for display in config.getDisplays():
        views = list(config.getViews(display))
        print(f"  {display}: {views}")

    return all_cs


def main():
    parser = argparse.ArgumentParser(description="Generate ACES 2.0 3D LUT binaries")
    parser.add_argument(
        "--size", type=int, default=128, help="LUT resolution per axis (default: 128)"
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="luts",
        help="Output directory (default: luts/)",
    )
    parser.add_argument(
        "--list-colorspaces",
        action="store_true",
        help="List available colorspaces and exit",
    )
    parser.add_argument(
        "--config",
        type=str,
        default=None,
        help="Path to OCIO config (default: ACES 2.0 built-in)",
    )
    args = parser.parse_args()

    # Load ACES 2.0 built-in config
    if args.config:
        config = ocio.Config.CreateFromFile(args.config)
    else:
        # Use OCIO built-in ACES 2.0 config (requires OCIO >= 2.4)
        builtin_names = [
            # Prefer the ACES v2.0 studio config (OCIO 2.5+)
            "studio-config-v4.0.0_aces-v2.0_ocio-v2.5",
            # Fallback: ACES v1.3 studio config (OCIO 2.4)
            "studio-config-v2.1.0_aces-v1.3_ocio-v2.4",
            "studio-config-v2.2.0_aces-v1.3_ocio-v2.4",
            # CG configs as last resort
            "cg-config-v4.0.0_aces-v2.0_ocio-v2.5",
            "cg-config-v2.2.0_aces-v1.3_ocio-v2.4",
        ]
        config = None
        for name in builtin_names:
            try:
                config = ocio.Config.CreateFromBuiltinConfig(name)
                print(f"Using built-in config: {name}")
                break
            except Exception:
                continue
        if config is None:
            print("ERROR: Could not load any ACES built-in config.", file=sys.stderr)
            print("  Ensure PyOpenColorIO >= 2.4.2 is installed.", file=sys.stderr)
            print("  Or provide a config file via --config.", file=sys.stderr)
            sys.exit(1)

    if args.list_colorspaces:
        find_aces2_colorspaces(config)
        return

    os.makedirs(args.output_dir, exist_ok=True)

    lut_size = args.size
    print(f"Generating {lut_size}^3 ACES 2.0 LUTs...")
    print(f"Shaper range: 2^{SHAPER_LOG2_MIN} to 2^{SHAPER_LOG2_MAX}")
    print(f"  = [{2.0**SHAPER_LOG2_MIN:.12f}, {2.0**SHAPER_LOG2_MAX:.1f}]")
    print()

    # --- 1000-nit HDR (Rec.2100 PQ) ---
    # The ACES 2.0 Output Transform: ACES2065-1 -> Rec.2100-PQ 1000 nits
    # In the studio config, this is typically:
    #   Source: "ACES2065-1" (scene-linear)
    #   Display: "Rec.2100-PQ" with View: "ACES 2.0 - SDR Video" or similar
    # We need to use the display/view transform system.

    # Try display-view approach first
    hdr_transforms = [
        # (display, view) pairs to try for 1000-nit HDR
        # ACES v2.0 (OCIO 2.5+)
        ("Rec.2100-PQ - Display", "ACES 2.0 - HDR 1000 nits (Rec.2020)"),
        ("Rec.2100-PQ - Display", "ACES 2.0 - HDR 1000 nits (P3 D65)"),
        # ACES v1.3 fallbacks
        ("Rec.2100-PQ", "ACES 2.0 - SDR Video"),
        ("Rec.2100-PQ", "ACES 1.0 - SDR Video"),
    ]

    sdr_transforms = [
        # (display, view) pairs to try for SDR
        # ACES v2.0 (OCIO 2.5+)
        ("sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)"),
        # ACES v1.3 fallbacks
        ("sRGB - Display", "ACES 2.0 - SDR Video"),
        ("sRGB", "ACES 2.0 - SDR Video"),
        ("sRGB - Display", "ACES 1.0 - SDR Video"),
        ("sRGB", "ACES 1.0 - SDR Video"),
    ]

    # List displays and views
    print("Available displays:")
    for display in config.getDisplays():
        views = list(config.getViews(display))
        print(f"  {display}: {', '.join(views)}")
    print()

    # Source colorspace
    src_cs = "ACES2065-1"

    # Generate HDR 1000-nit forward LUT
    print("--- HDR 1000-nit (Rec.2100 PQ) ---")
    hdr_forward_proc = None
    for display, view in hdr_transforms:
        try:
            hdr_forward_proc = config.getProcessor(
                src_cs, display, view, ocio.TRANSFORM_DIR_FORWARD
            )
            print(f"  Using display={display}, view={view}")
            break
        except Exception as e:
            continue

    if hdr_forward_proc is None:
        # Fallback: try colorspace-to-colorspace
        try:
            hdr_forward_proc = config.getProcessor(src_cs, "Rec.2100-PQ - Display")
            print(f"  Using colorspace: ACES2065-1 -> Rec.2100-PQ - Display")
        except Exception:
            print(
                "ERROR: Could not find HDR 1000-nit output transform.", file=sys.stderr
            )
            print(
                "  Run with --list-colorspaces to see available options.",
                file=sys.stderr,
            )
            sys.exit(1)

    print(f"  Generating forward LUT ({lut_size}^3)...")
    hdr_forward_lut = generate_3d_lut(hdr_forward_proc, lut_size)
    write_binary_lut(
        os.path.join(args.output_dir, "aces2_1000nit_forward.bin"), hdr_forward_lut
    )

    # Generate HDR 1000-nit inverse LUT
    print(f"  Generating inverse LUT ({lut_size}^3)...")
    # For inverse: we need display -> scene-linear
    # The inverse processor maps display-referred values back
    hdr_inverse_proc = None
    for display, view in hdr_transforms:
        try:
            hdr_inverse_proc = config.getProcessor(
                src_cs, display, view, ocio.TRANSFORM_DIR_FORWARD
            )
            # Get the inverse
            hdr_inverse_proc = hdr_inverse_proc.createGroupTransform()
            # Actually, let's use INVERSE direction
            hdr_inverse_proc = config.getProcessor(
                src_cs, display, view, ocio.TRANSFORM_DIR_INVERSE
            )
            break
        except Exception:
            continue

    if hdr_inverse_proc is None:
        try:
            proc = config.getProcessor("Rec.2100-PQ - Display", src_cs)
            hdr_inverse_proc = proc
        except Exception:
            print("ERROR: Could not create inverse HDR transform.", file=sys.stderr)
            sys.exit(1)

    # For inverse LUT, the input is in display-referred space [0, 1].
    # The GPU shader will sample this LUT directly with clamped RGB coords (no log2 shaper).
    # So we generate the LUT on a uniform [0, 1]^3 display-domain lattice.
    hdr_inverse_lut = generate_3d_lut_display_domain(hdr_inverse_proc, lut_size)
    write_binary_lut(
        os.path.join(args.output_dir, "aces2_1000nit_inverse.bin"), hdr_inverse_lut
    )
    print()

    # --- SDR (sRGB 100-nit) ---
    print("--- SDR (sRGB 100-nit) ---")
    sdr_forward_proc = None
    for display, view in sdr_transforms:
        try:
            sdr_forward_proc = config.getProcessor(
                src_cs, display, view, ocio.TRANSFORM_DIR_FORWARD
            )
            print(f"  Using display={display}, view={view}")
            break
        except Exception:
            continue

    if sdr_forward_proc is None:
        try:
            sdr_forward_proc = config.getProcessor(src_cs, "sRGB - Display")
            print(f"  Using colorspace: ACES2065-1 -> sRGB - Display")
        except Exception:
            print(
                "WARNING: Could not find SDR output transform. Skipping SDR LUTs.",
                file=sys.stderr,
            )
            return

    print(f"  Generating forward LUT ({lut_size}^3)...")
    sdr_forward_lut = generate_3d_lut(sdr_forward_proc, lut_size)
    write_binary_lut(
        os.path.join(args.output_dir, "aces2_sdr_forward.bin"), sdr_forward_lut
    )

    print(f"  Generating inverse LUT ({lut_size}^3)...")
    sdr_inverse_proc = None
    for display, view in sdr_transforms:
        try:
            sdr_inverse_proc = config.getProcessor(
                src_cs, display, view, ocio.TRANSFORM_DIR_INVERSE
            )
            break
        except Exception:
            continue

    if sdr_inverse_proc is None:
        try:
            sdr_inverse_proc = config.getProcessor("sRGB - Display", src_cs)
        except Exception:
            print(
                "WARNING: Could not create inverse SDR transform. Skipping.",
                file=sys.stderr,
            )
            return

    sdr_inverse_lut = generate_3d_lut_display_domain(sdr_inverse_proc, lut_size)
    write_binary_lut(
        os.path.join(args.output_dir, "aces2_sdr_inverse.bin"), sdr_inverse_lut
    )

    print()
    print("Done! LUT files written to:", args.output_dir)
    print()
    print("Parameters (needed for C++ integration):")
    print(f"  SHAPER_LOG2_MIN = {SHAPER_LOG2_MIN}  (forward LUT only)")
    print(f"  SHAPER_LOG2_MAX = {SHAPER_LOG2_MAX}  (forward LUT only)")
    print(f"  LUT_SIZE = {lut_size}")
    print()
    print("Forward LUTs use log2 shaper domain [2^min, 2^max] -> [0, 1].")
    print("Inverse LUTs use display-domain [0, 1]^3 directly (no shaper).")


if __name__ == "__main__":
    main()
