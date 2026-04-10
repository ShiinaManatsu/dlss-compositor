from __future__ import annotations

import struct
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
FIXTURES = ROOT / "fixtures"
SEQUENCE = FIXTURES / "sequence"
BUILD_FIXTURES = ROOT.parent / "build" / "tests" / "tests" / "fixtures"
BUILD_SEQUENCE = BUILD_FIXTURES / "sequence"

WIDTH = 64
HEIGHT = 64


def _channel_order():
    return [
        "RenderLayer.Combined.R",
        "RenderLayer.Combined.G",
        "RenderLayer.Combined.B",
        "RenderLayer.Combined.A",
        "RenderLayer.Depth.Z",
        "RenderLayer.Vector.X",
        "RenderLayer.Vector.Y",
        "RenderLayer.Vector.Z",
        "RenderLayer.Vector.W",
        "RenderLayer.Normal.X",
        "RenderLayer.Normal.Y",
        "RenderLayer.Normal.Z",
        "RenderLayer.DiffCol.R",
        "RenderLayer.DiffCol.G",
        "RenderLayer.DiffCol.B",
        "RenderLayer.GlossCol.R",
        "RenderLayer.GlossCol.G",
        "RenderLayer.GlossCol.B",
        "RenderLayer.Roughness.X",
    ]


def _missing_channel_order():
    return [
        "RenderLayer.Combined.R",
        "RenderLayer.Combined.G",
        "RenderLayer.Combined.B",
        "RenderLayer.Combined.A",
        "RenderLayer.Depth.Z",
        "RenderLayer.Vector.X",
        "RenderLayer.Vector.Y",
        "RenderLayer.Vector.Z",
        "RenderLayer.Vector.W",
    ]


def _write_attr(fp, name: str, type_name: str, payload: bytes) -> None:
    fp.write(name.encode("ascii") + b"\x00")
    fp.write(type_name.encode("ascii") + b"\x00")
    fp.write(struct.pack("<I", len(payload)))
    fp.write(payload)


def _channels_payload(names: list[str]) -> bytes:
    chunks = []
    for name in names:
        chunks.append(name.encode("ascii") + b"\x00")
        chunks.append(struct.pack("<iB3xii", 2, 0, 1, 1))
    chunks.append(b"\x00")
    return b"".join(chunks)


def _header_bytes(width: int, height: int, names: list[str]) -> bytes:
    payload = bytearray()
    _write_attr_stream(payload, "channels", "chlist", _channels_payload(names))
    _write_attr_stream(payload, "compression", "compression", struct.pack("<B", 0))
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    _write_attr_stream(payload, "dataWindow", "box2i", box)
    _write_attr_stream(payload, "displayWindow", "box2i", box)
    _write_attr_stream(payload, "lineOrder", "lineOrder", struct.pack("<B", 0))
    _write_attr_stream(payload, "pixelAspectRatio", "float", struct.pack("<f", 1.0))
    _write_attr_stream(
        payload, "screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0)
    )
    _write_attr_stream(payload, "screenWindowWidth", "float", struct.pack("<f", 1.0))
    payload.append(0)
    return bytes(payload)


def _write_attr_stream(
    payload: bytearray, name: str, type_name: str, value: bytes
) -> None:
    payload.extend(name.encode("ascii") + b"\x00")
    payload.extend(type_name.encode("ascii") + b"\x00")
    payload.extend(struct.pack("<I", len(value)))
    payload.extend(value)


def _combine_r(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    return (x * y).astype(np.float32)


def _depth_ramp(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    return (0.1 + ((x + y) / 126.0) * 0.9).astype(np.float32)


def _build_reference_image(frame_index: int | None = None) -> dict[str, np.ndarray]:
    xs = np.linspace(0.0, 1.0, WIDTH, dtype=np.float32)
    ys = np.linspace(0.0, 1.0, HEIGHT, dtype=np.float32)
    xg, yg = np.meshgrid(xs, ys)

    combined_r = _combine_r(xg, yg)
    combined_g = xg.astype(np.float32)
    combined_b = yg.astype(np.float32)
    combined_a = np.ones((HEIGHT, WIDTH), dtype=np.float32)
    depth = _depth_ramp(xg, yg)

    if frame_index is None:
        mv_x = np.full((HEIGHT, WIDTH), 0.1, dtype=np.float32)
    else:
        mv_x = np.full((HEIGHT, WIDTH), frame_index * 0.1, dtype=np.float32)
    mv_y = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    mv_z = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    mv_w = np.zeros((HEIGHT, WIDTH), dtype=np.float32)

    normal_x = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    normal_y = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    normal_z = np.ones((HEIGHT, WIDTH), dtype=np.float32)

    diff_r = np.ones((HEIGHT, WIDTH), dtype=np.float32)
    diff_g = np.ones((HEIGHT, WIDTH), dtype=np.float32)
    diff_b = np.ones((HEIGHT, WIDTH), dtype=np.float32)

    gloss_r = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    gloss_g = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    gloss_b = np.zeros((HEIGHT, WIDTH), dtype=np.float32)

    roughness = np.full((HEIGHT, WIDTH), 0.5, dtype=np.float32)

    return {
        "RenderLayer.Combined.R": combined_r,
        "RenderLayer.Combined.G": combined_g,
        "RenderLayer.Combined.B": combined_b,
        "RenderLayer.Combined.A": combined_a,
        "RenderLayer.Depth.Z": depth,
        "RenderLayer.Vector.X": mv_x,
        "RenderLayer.Vector.Y": mv_y,
        "RenderLayer.Vector.Z": mv_z,
        "RenderLayer.Vector.W": mv_w,
        "RenderLayer.Normal.X": normal_x,
        "RenderLayer.Normal.Y": normal_y,
        "RenderLayer.Normal.Z": normal_z,
        "RenderLayer.DiffCol.R": diff_r,
        "RenderLayer.DiffCol.G": diff_g,
        "RenderLayer.DiffCol.B": diff_b,
        "RenderLayer.GlossCol.R": gloss_r,
        "RenderLayer.GlossCol.G": gloss_g,
        "RenderLayer.GlossCol.B": gloss_b,
        "RenderLayer.Roughness.X": roughness,
    }


def _build_missing_image() -> dict[str, np.ndarray]:
    xs = np.linspace(0.0, 1.0, WIDTH, dtype=np.float32)
    ys = np.linspace(0.0, 1.0, HEIGHT, dtype=np.float32)
    xg, yg = np.meshgrid(xs, ys)
    return {
        "RenderLayer.Combined.R": _combine_r(xg, yg),
        "RenderLayer.Combined.G": xg.astype(np.float32),
        "RenderLayer.Combined.B": yg.astype(np.float32),
        "RenderLayer.Combined.A": np.ones((HEIGHT, WIDTH), dtype=np.float32),
        "RenderLayer.Depth.Z": _depth_ramp(xg, yg),
        "RenderLayer.Vector.X": np.full((HEIGHT, WIDTH), 0.1, dtype=np.float32),
        "RenderLayer.Vector.Y": np.zeros((HEIGHT, WIDTH), dtype=np.float32),
        "RenderLayer.Vector.Z": np.zeros((HEIGHT, WIDTH), dtype=np.float32),
        "RenderLayer.Vector.W": np.zeros((HEIGHT, WIDTH), dtype=np.float32),
    }


def write_exr(path: Path, channels: list[str], image: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    header = _header_bytes(WIDTH, HEIGHT, channels)
    scanline_block_size = WIDTH * len(channels) * 4
    offsets_start = 8 + len(header)
    blocks_start = offsets_start + 8 * HEIGHT

    with path.open("wb") as fp:
        fp.write(b"\x76\x2f\x31\x01")
        fp.write(struct.pack("<I", 2))
        fp.write(header)

        for y in range(HEIGHT):
            fp.write(struct.pack("<Q", blocks_start + y * (8 + scanline_block_size)))

        for y in range(HEIGHT):
            fp.write(struct.pack("<iI", y, scanline_block_size))
            for name in channels:
                row = image[name][y].astype("<f4", copy=False)
                fp.write(row.tobytes())


def main() -> int:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    SEQUENCE.mkdir(parents=True, exist_ok=True)
    BUILD_FIXTURES.mkdir(parents=True, exist_ok=True)
    BUILD_SEQUENCE.mkdir(parents=True, exist_ok=True)

    write_exr(
        FIXTURES / "reference_64x64.exr", _channel_order(), _build_reference_image()
    )
    write_exr(
        FIXTURES / "missing_channels_64x64.exr",
        _missing_channel_order(),
        _build_missing_image(),
    )
    write_exr(
        BUILD_FIXTURES / "reference_64x64.exr",
        _channel_order(),
        _build_reference_image(),
    )
    write_exr(
        BUILD_FIXTURES / "missing_channels_64x64.exr",
        _missing_channel_order(),
        _build_missing_image(),
    )

    for frame in range(1, 6):
        image = _build_reference_image(frame_index=frame)
        write_exr(
            SEQUENCE / f"frame_{frame:04d}.exr",
            _channel_order(),
            image,
        )
        write_exr(
            BUILD_SEQUENCE / f"frame_{frame:04d}.exr",
            _channel_order(),
            image,
        )

    readme = FIXTURES / "README.md"
    readme.write_text(
        "# Fixture EXRs\n\n"
        "Generated by `tests/generate_fixtures.py`. All files are 64x64, single-part, scanline, uncompressed EXR files.\n\n"
        "## `reference_64x64.exr`\n\n"
        "Channels:\n"
        "- `RenderLayer.Combined.R/G/B/A`\n"
        "- `RenderLayer.Depth.Z`\n"
        "- `RenderLayer.Vector.X/Y/Z/W`\n"
        "- `RenderLayer.Normal.X/Y/Z`\n"
        "- `RenderLayer.DiffCol.R/G/B`\n"
        "- `RenderLayer.GlossCol.R/G/B`\n"
        "- `RenderLayer.Roughness.X`\n\n"
        "Known pixel values:\n"
        "- `(0,0)` Combined.R = `0.0`\n"
        "- `(31,31)` Combined.R = `0.2420`\n"
        "- `(0,0)` Depth.Z = `0.1`\n"
        "- `(31,31)` Depth.Z = `0.5429`\n"
        "- DiffCol.R = `1.0` everywhere\n"
        "- GlossCol.RGB = `0.0` everywhere\n"
        "- Normal.Z = `1.0` everywhere\n"
        "- Roughness.X = `0.5` everywhere\n\n"
        "## `missing_channels_64x64.exr`\n\n"
        "Contains only Combined RGBA, Depth.Z, and Vector XYZW. The remaining optional Blender channels are absent so mapper defaults can be tested.\n\n"
        "## `sequence/frame_0001.exr` ... `frame_0005.exr`\n\n"
        "Same channel layout as the reference fixture, but `RenderLayer.Vector.X` changes per frame: `0.1`, `0.2`, `0.3`, `0.4`, `0.5`. All other channels remain synthetic and deterministic.\n"
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
