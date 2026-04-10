#!/usr/bin/env python3
"""EXR validator for DLSS Compositor workflow."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REQUIRED_CHANNELS = {
    "Combined.R",
    "Combined.G",
    "Combined.B",
    "Combined.A",
    "Depth.Z",
    "Vector.X",
    "Vector.Y",
    "Vector.Z",
    "Vector.W",
    "Normal.X",
    "Normal.Y",
    "Normal.Z",
}

OPTIONAL_CHANNELS = {
    "DiffCol.R",
    "DiffCol.G",
    "DiffCol.B",
    "GlossCol.R",
    "GlossCol.G",
    "GlossCol.B",
    "Roughness.X",
}

OUTPUT_CHANNELS = {"R", "G", "B", "A"}


def _load_openexr():
    try:
        import OpenEXR  # type: ignore

        return OpenEXR
    except Exception as exc:  # pragma: no cover - runtime dependency issue
        print(f"FAIL: OpenEXR import failed: {exc}", file=sys.stderr)
        raise SystemExit(1)


def _pass_name(channel_name: str) -> str:
    parts = channel_name.split(".")
    if len(parts) >= 3:
        return parts[-2]
    return channel_name


def _matches(channel_name: str, expected_suffix: str) -> bool:
    return (
        channel_name == expected_suffix
        or channel_name.endswith(f".{expected_suffix}")
        or channel_name.endswith(expected_suffix)
    )


def _channel_groups(
    channel_names: list[str], expected_suffixes: set[str]
) -> dict[str, list[str]]:
    grouped: dict[str, list[str]] = {suffix: [] for suffix in expected_suffixes}
    for name in channel_names:
        for suffix in expected_suffixes:
            if _matches(name, suffix):
                grouped[suffix].append(name)
    return grouped


def _validate_single_file(path: Path, strict: bool) -> bool:
    OpenEXR = _load_openexr()
    try:
        exr = OpenEXR.InputFile(str(path))
    except Exception as exc:
        print(f"FAIL: {path}: {exc}")
        return False

    try:
        header = exr.header()
        channels = header.get("channels", {})
        channel_names = list(channels.keys())
        data_window = header.get("dataWindow")
        expected_width = None
        expected_height = None
        if data_window is not None:
            expected_width = data_window.max.x - data_window.min.x + 1
            expected_height = data_window.max.y - data_window.min.y + 1

        print(f"FILE: {path}")
        print(f"Channels: {', '.join(channel_names) if channel_names else '(none)'}")
        if expected_width is not None and expected_height is not None:
            print(f"Resolution: {expected_width}x{expected_height}")

        if OUTPUT_CHANNELS.issubset(set(channel_names)):
            print("PASS output RGBA channels: processed output detected")
            if expected_width is not None and expected_height is not None:
                if expected_width <= 0 or expected_height <= 0:
                    print("FAIL: invalid resolution in dataWindow")
                    return False
                print("PASS resolution check")
            print("PASS: Processed output EXR is valid")
            return True

        required_ok = True
        missing: list[str] = []
        for suffix in sorted(REQUIRED_CHANNELS):
            found = [name for name in channel_names if _matches(name, suffix)]
            if found:
                print(
                    f"PASS required {_pass_name(found[0])}.{suffix.split('.')[-1]}: {found[0]}"
                )
            else:
                print(f"FAIL required {suffix}: missing")
                required_ok = False
                missing.append(suffix)

        if strict:
            for suffix in sorted(OPTIONAL_CHANNELS):
                found = [name for name in channel_names if _matches(name, suffix)]
                if found:
                    print(
                        f"PASS optional {_pass_name(found[0])}.{suffix.split('.')[-1]}: {found[0]}"
                    )
                else:
                    print(f"FAIL optional {suffix}: missing")
                    required_ok = False
                    missing.append(suffix)

        if expected_width is not None and expected_height is not None:
            if expected_width <= 0 or expected_height <= 0:
                print("FAIL: invalid resolution in dataWindow")
                required_ok = False
            else:
                print("PASS resolution check")

        if required_ok:
            print("PASS: All required channels present")
            return True

        print(f"FAIL: Missing channels: {missing}")
        return False
    finally:
        try:
            exr.close()
        except Exception:
            pass


def _collect_exr_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path] if path.suffix.lower() == ".exr" else []
    exrs: list[Path] = []
    for root, _, files in os.walk(path):
        for name in files:
            if name.lower().endswith(".exr"):
                exrs.append(Path(root) / name)
    return sorted(exrs)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate EXR channel structure.")
    parser.add_argument("exr_file_or_dir", help="EXR file or directory to validate")
    parser.add_argument(
        "--strict", action="store_true", help="Require optional channels too"
    )
    args = parser.parse_args(argv)

    target = Path(args.exr_file_or_dir)
    if not target.exists():
        print(f"FAIL: path not found: {target}")
        return 1

    files = _collect_exr_files(target)
    if not files:
        print(f"FAIL: no EXR files found in {target}")
        return 1

    ok = True
    for file_path in files:
        ok = _validate_single_file(file_path, args.strict) and ok

    if len(files) > 1:
        print(f"SUMMARY: validated {len(files)} EXR file(s)")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
