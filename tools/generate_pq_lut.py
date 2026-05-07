#!/usr/bin/env python3
"""Generate PQ (ST 2084) forward and inverse 3D LUT binaries for dlss-compositor.

Requires: NumPy >= 1.20.0

Usage:
    python tools/generate_pq_lut.py [--size 64] [--output-dir assets/]

Outputs:
    pq_forward.bin   -- scene-linear [0, 100] -> PQ code value [0, 1]
    pq_inverse.bin   -- PQ code value [0, 1] -> scene-linear [0, 100]
    pq_lut_meta.json -- metadata describing the LUT parameters

Binary format:
    Raw float32, RGBA interleaved, size^3 * 4 * 4 bytes. No file header.
    Lattice order: X (R) varies fastest, Z (B) varies slowest.
    This matches the row-major 3D indexing expected by lut_processor.cpp.

PQ constants from SMPTE ST 2084 (identical to src/shaders/pq_transfer.comp):
    m1 = 2610 / 16384      = 0.1593017578125
    m2 = 2523 / 4096 * 128 = 78.84375
    c1 = 3424 / 4096       = 0.8359375
    c2 = 2413 / 4096 * 32  = 18.8515625
    c3 = 2392 / 4096 * 32  = 18.6875
    SDR_WHITE_NITS = 100.0
    PQ_MAX_NITS    = 10000.0
"""

import argparse
import json
import os
import sys

import numpy as np

# ---------------------------------------------------------------------------
# ST 2084 constants — MUST match src/shaders/pq_transfer.comp exactly
# ---------------------------------------------------------------------------
M1 = 2610.0 / 16384.0          # 0.1593017578125
M2 = 2523.0 / 4096.0 * 128.0  # 78.84375
C1 = 3424.0 / 4096.0           # 0.8359375
C2 = 2413.0 / 4096.0 * 32.0   # 18.8515625
C3 = 2392.0 / 4096.0 * 32.0   # 18.6875
SDR_WHITE_NITS = 100.0
PQ_MAX_NITS = 10000.0


def linear_to_pq(L: np.ndarray) -> np.ndarray:
    """Encode scene-linear luminance to PQ code value [0, 1].

    Args:
        L: Scene-linear value(s) where 1.0 = 100 nits (SDR reference white).

    Returns:
        PQ-encoded value(s) in [0, 1], matching linearToPQ() in pq_transfer.comp.
    """
    Lp = np.clip(L * SDR_WHITE_NITS / PQ_MAX_NITS, 0.0, 1.0)
    Lm1 = np.power(Lp, M1)
    num = C1 + C2 * Lm1
    den = 1.0 + C3 * Lm1
    return np.power(num / den, M2)


def pq_to_linear(N: np.ndarray) -> np.ndarray:
    """Decode PQ code value to scene-linear luminance.

    Args:
        N: PQ-encoded value(s) in [0, 1].

    Returns:
        Scene-linear value(s) where 1.0 = 100 nits, matching pqToLinear() in
        pq_transfer.comp.
    """
    Np = np.clip(N, 0.0, 1.0)
    Nm2 = np.power(Np, 1.0 / M2)
    num = np.maximum(Nm2 - C1, 0.0)
    den = C2 - C3 * Nm2
    Lp = np.power(num / np.maximum(den, 1e-10), 1.0 / M1)
    return Lp * PQ_MAX_NITS / SDR_WHITE_NITS


def generate_lut(size: int, forward: bool) -> np.ndarray:
    """Generate a 3D LUT as a flat float32 RGBA array.

    Lattice order: Z varies slowest, Y middle, X fastest — matching the
    row-major layout expected by lut_processor.cpp (index = z*size*size + y*size + x).

    Args:
        size:    Number of grid points per axis.
        forward: True  → scene-linear input, PQ output (forward encode).
                 False → PQ input, scene-linear output (inverse decode).

    Returns:
        NumPy array of shape (size^3 * 4,) in float32.
    """
    direction = "forward (linear → PQ)" if forward else "inverse (PQ → linear)"
    print(f"  Building {size}^3 {direction} LUT …", end="", flush=True)

    # Build normalised coordinate grids [0, 1] for each axis
    coords = np.linspace(0.0, 1.0, size, dtype=np.float64)

    # Grid layout: Z slowest, Y middle, X fastest
    # np.meshgrid with indexing='ij' gives (size, size, size) for (Z, Y, X)
    gz, gy, gx = np.meshgrid(coords, coords, coords, indexing="ij")

    # Apply the PQ transfer function per-channel
    if forward:
        out_x = linear_to_pq(gx)
        out_y = linear_to_pq(gy)
        out_z = linear_to_pq(gz)
    else:
        out_x = pq_to_linear(gx)
        out_y = pq_to_linear(gy)
        out_z = pq_to_linear(gz)

    # Alpha channel is always 1.0
    alpha = np.ones_like(out_x)

    # Interleave as RGBA: shape (size, size, size, 4), then flatten
    rgba = np.stack([out_x, out_y, out_z, alpha], axis=-1)  # (Z, Y, X, 4)
    flat = rgba.flatten().astype(np.float32)

    print(f" done ({flat.nbytes:,} bytes)")
    return flat


def write_lut(data: np.ndarray, filepath: str) -> None:
    """Write LUT data as raw binary float32 to *filepath*."""
    data.tofile(filepath)
    print(f"  Written: {filepath}  ({os.path.getsize(filepath):,} bytes)")


def write_metadata(size: int, output_dir: str) -> None:
    """Write a JSON sidecar with LUT generation parameters."""
    meta = {
        "generator": "generate_pq_lut.py",
        "spec": "SMPTE ST 2084 (PQ / Perceptual Quantizer)",
        "lut_size": size,
        "file_size_bytes": size ** 3 * 4 * 4,
        "format": "raw float32 RGBA, no header",
        "lattice_order": "Z slowest, Y middle, X fastest (row-major)",
        "pq_constants": {
            "m1": M1,
            "m2": M2,
            "c1": C1,
            "c2": C2,
            "c3": C3,
            "SDR_WHITE_NITS": SDR_WHITE_NITS,
            "PQ_MAX_NITS": PQ_MAX_NITS,
        },
        "files": {
            "forward": "pq_forward.bin  (scene-linear -> PQ code value [0,1])",
            "inverse": "pq_inverse.bin  (PQ code value [0,1] -> scene-linear)",
        },
    }
    meta_path = os.path.join(output_dir, "pq_lut_meta.json")
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
    print(f"  Metadata: {meta_path}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate PQ (ST 2084) 3D LUT .bin files for dlss-compositor.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--size",
        type=int,
        default=64,
        metavar="N",
        help="Grid points per axis (output is N^3 texels). Common values: 16, 33, 64.",
    )
    parser.add_argument(
        "--output-dir",
        default="assets/",
        metavar="DIR",
        help="Directory to write pq_forward.bin and pq_inverse.bin.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    size = args.size
    output_dir = args.output_dir

    if size < 2:
        print("ERROR: --size must be >= 2", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    expected_bytes = size ** 3 * 4 * 4
    print(f"PQ LUT generator — size={size}, output_dir={output_dir!r}")
    print(f"Expected file size per LUT: {expected_bytes:,} bytes ({size}^3 * 4 channels * 4 bytes)")
    print()

    # --- Forward LUT: scene-linear → PQ ---
    print("Generating forward LUT (scene-linear → PQ) …")
    fwd_data = generate_lut(size, forward=True)
    fwd_path = os.path.join(output_dir, "pq_forward.bin")
    write_lut(fwd_data, fwd_path)

    # Validate file size
    actual = os.path.getsize(fwd_path)
    assert actual == expected_bytes, f"Forward LUT size mismatch: {actual} != {expected_bytes}"
    print()

    # --- Inverse LUT: PQ → scene-linear ---
    print("Generating inverse LUT (PQ → scene-linear) …")
    inv_data = generate_lut(size, forward=False)
    inv_path = os.path.join(output_dir, "pq_inverse.bin")
    write_lut(inv_data, inv_path)

    actual = os.path.getsize(inv_path)
    assert actual == expected_bytes, f"Inverse LUT size mismatch: {actual} != {expected_bytes}"
    print()

    # --- Metadata ---
    write_metadata(size, output_dir)
    print()
    print("Done.")


if __name__ == "__main__":
    main()
