# LUT Generation and Custom Tonemap Workflow

This guide explains how to generate PQ (ST 2084) 3D LUT files for the
dlss-compositor custom tonemap pipeline, and how the files are consumed at
runtime by `lut_processor.cpp`.

---

## Introduction

DLSS Compositor ships with a built-in PQ encode/decode compute shader
(`src/shaders/pq_transfer.comp`).  For advanced workflows — such as baking a
custom display transform on top of PQ, or pre-computing LUTs to reduce
per-frame GPU overhead — you can supply your own **3D LUT `.bin` files**.

The Python script `tools/generate_pq_lut.py` generates the reference PQ
forward and inverse LUTs that ship with the compositor.

---

## When to Use LUT vs PQ

| Scenario | Recommended approach |
|---|---|
| Standard HDR renders (ST 2084 round-trip) | Built-in shader — no LUT needed |
| Custom display transform (e.g. ACES on top of PQ) | Generate a custom LUT with your colour pipeline |
| Offline batch processing with pre-computed tonemap | Generate LUT once, reuse across all frames |
| Debugging / verifying the PQ formula | Use the reference LUTs from this script |

---

## Generating LUTs

### Prerequisites

Install Python dependencies:

```powershell
pip install -r requirements.txt
```

NumPy is the only dependency required by the PQ LUT generator.

### Generating the Reference PQ LUTs

```powershell
# Default: size=64, output to assets/
python tools/generate_pq_lut.py

# Custom grid size (larger = more precise, larger file)
python tools/generate_pq_lut.py --size 33 --output-dir assets/

# Size 16 for quick testing
python tools/generate_pq_lut.py --size 16 --output-dir test_lut_assets/
```

Output files:

| File | Content |
|---|---|
| `assets/pq_forward.bin` | Scene-linear → PQ code value `[0, 1]` |
| `assets/pq_inverse.bin` | PQ code value `[0, 1]` → scene-linear |
| `assets/pq_lut_meta.json` | Metadata: size, constants, spec reference |

### File Size

```
file_size_bytes = size³ × 4 channels × 4 bytes per float
```

Examples:

| `--size` | File size |
|---|---|
| 16 | 65,536 bytes (64 KB) |
| 33 | 576,036 bytes (562 KB) |
| 64 | 4,194,304 bytes (4 MB) |

---

## CLI Usage

```powershell
python tools/generate_pq_lut.py --help
```

```
usage: generate_pq_lut.py [-h] [--size N] [--output-dir DIR]

Generate PQ (ST 2084) 3D LUT .bin files for dlss-compositor.

optional arguments:
  -h, --help         show this help message and exit
  --size N           Grid points per axis (default: 64)
  --output-dir DIR   Directory to write output files (default: assets/)
```

### Using Custom LUTs at Runtime

Pass the LUT files to the compositor CLI with `--tonemap-lut` and
`--inverse-tonemap-lut`:

```powershell
dlss-compositor.exe \
  --input-dir renders/ \
  --output-dir output/ \
  --interpolate 2x \
  --camera-data camera.json \
  --tonemap-lut assets/pq_forward.bin \
  --inverse-tonemap-lut assets/pq_inverse.bin
```

The compositor validates the file size on load.  If `--size 64` was used, pass
`--lut-size 64` (or the default is assumed to be 64).

---

## Technical Reference

### PQ Constants (SMPTE ST 2084)

These constants are identical in `src/shaders/pq_transfer.comp` and
`tools/generate_pq_lut.py`:

| Constant | Value | Derivation |
|---|---|---|
| `m1` | `0.1593017578125` | `2610 / 16384` |
| `m2` | `78.84375` | `2523 / 4096 × 128` |
| `c1` | `0.8359375` | `3424 / 4096` |
| `c2` | `18.8515625` | `2413 / 4096 × 32` |
| `c3` | `18.6875` | `2392 / 4096 × 32` |
| `SDR_WHITE_NITS` | `100.0` | sRGB reference white |
| `PQ_MAX_NITS` | `10000.0` | PQ peak luminance |

### Transfer Functions

**Encode (scene-linear → PQ):**

```
Lp  = clamp(L × SDR_WHITE_NITS / PQ_MAX_NITS, 0, 1)
Lm1 = Lp ^ m1
N   = ((c1 + c2 × Lm1) / (1 + c3 × Lm1)) ^ m2
```

**Decode (PQ → scene-linear):**

```
Np  = clamp(N, 0, 1)
Nm2 = Np ^ (1/m2)
Lp  = (max(Nm2 - c1, 0) / max(c2 - c3 × Nm2, 1e-10)) ^ (1/m1)
L   = Lp × PQ_MAX_NITS / SDR_WHITE_NITS
```

### Binary File Format

- **No file header** — raw binary data only.
- **Element type**: `float32` (IEEE 754 single precision), little-endian.
- **Channels**: RGBA interleaved.  Alpha is always `1.0`.
- **Lattice order**: **X varies fastest, Z varies slowest** (row-major 3D array).
  - Index formula: `texel = z × size² + y × size + x`
  - This matches the `lut_processor.cpp` `loadLut()` expectation.
- **Coordinate mapping**: axis index `i` maps to normalised value `i / (size - 1)`.

### Running the Tests

```powershell
pytest tests/test_pq_lut_generator.py -v
```

The test suite:
- Verifies exact file size for `--size 16` output.
- Confirms the forward→inverse roundtrip error is < 2 %.
- Confirms nearest-neighbour LUT lookup error vs CPU reference is < 2 % at `--size 64`.
- Validates the JSON metadata sidecar.
