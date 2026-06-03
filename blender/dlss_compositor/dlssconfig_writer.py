"""
DLSS Compositor dlssconfig.json writer — Blender side.
Schema single source of truth: src/core/dlssconfig.schema.json
This module builds, validates, and atomically writes dlssconfig.json.
"""
from __future__ import annotations

import importlib.util
import json
import logging
import os
from pathlib import Path
from typing import Any

from bpy.types import Scene

log = logging.getLogger("dlss_compositor.dlssconfig_writer")

# HDR display devices per Blender 5.1 docs
HDR_DISPLAY_DEVICES = {"Rec.2100-PQ", "Rec.2100-HLG"}

# Path to schema relative to repo root — single source of truth
_SCHEMA_PATH = Path(__file__).parent.parent.parent / "src" / "core" / "dlssconfig.schema.json"

JITTER_CLAMP = 0.499


def _clamp_jitter(value: float, warn_name: str = "") -> float:
    """Clamp jitter to [-0.499, +0.499]. NEVER reject; always clamp + warn."""
    if value < -JITTER_CLAMP or value > JITTER_CLAMP:
        if warn_name:
            log.warning("jitter %s=%.6f out of range, clamping to ±0.499", warn_name, value)
    return max(-JITTER_CLAMP, min(JITTER_CLAMP, value))


def build_top_level_config(scene: Scene, props: object) -> dict[str, Any]:
    """Build the top-level dlssconfig dict from Blender scene + DLSS properties."""
    from .quality_mapping import JITTER_PHASES

    output_w = scene.render.resolution_x
    output_h = scene.render.resolution_y
    pct      = scene.render.resolution_percentage  # Already overridden by handler

    render_w = int(output_w * pct / 100)
    render_h = int(output_h * pct / 100)

    display_device = getattr(scene.display_settings, "display_device", "sRGB")
    view_transform = getattr(scene.view_settings, "view_transform", "Filmic")

    is_hdr        = display_device in HDR_DISPLAY_DEVICES
    quality_mode  = props.quality_mode.lower()  # type: ignore[attr-defined]
    jitter_phases = JITTER_PHASES[props.quality_mode]  # type: ignore[attr-defined]

    log.debug(
        "build_top_level_config: %dx%d→%dx%d quality=%s hdr=%s",
        output_w, output_h, render_w, render_h, quality_mode, is_hdr,
    )

    return {
        "version":           1,
        "output_resolution": {"width": output_w, "height": output_h},
        "render_resolution": {"width": render_w, "height": render_h},
        "quality_mode":      quality_mode,
        "display_device":    display_device,
        "view_transform":    view_transform,
        "is_hdr":            is_hdr,
        "fps":               scene.render.fps,
        "filename_pattern":  r"^\d{4}\.exr$",
        "jitter_phases":     jitter_phases,
    }


def merge_per_frame_jsons(output_dir: str) -> dict[str, Any]:
    """Scan output_dir/frames/*.cam.json and merge into a frames dict."""
    frames_dir = Path(output_dir) / "frames"
    frames: dict[str, Any] = {}

    if not frames_dir.exists():
        log.debug("merge_per_frame_jsons: frames dir does not exist: %s", frames_dir)
        return frames

    cam_files = sorted(frames_dir.glob("*.cam.json"))
    log.debug("merge_per_frame_jsons: found %d cam.json files", len(cam_files))

    for cam_file in cam_files:
        with open(cam_file, encoding="utf-8") as f:
            frame_data: dict[str, Any] = json.load(f)

        frame_key = cam_file.stem.replace(".cam", "")

        if "jitter_x" in frame_data:
            frame_data["jitter_x"] = _clamp_jitter(frame_data["jitter_x"], "jitter_x")
        if "jitter_y" in frame_data:
            frame_data["jitter_y"] = _clamp_jitter(frame_data["jitter_y"], "jitter_y")

        frames[frame_key] = frame_data
        log.debug("merge_per_frame_jsons: loaded frame %s", frame_key)

    return frames


def validate_against_schema(data: dict[str, Any]) -> None:
    """Validate dlssconfig dict against src/core/dlssconfig.schema.json.

    Skips silently if jsonschema is not installed or schema file is missing.
    Raises ValueError if validation fails.
    """
    if importlib.util.find_spec("jsonschema") is None:
        log.warning("validate_against_schema: jsonschema not installed — skipping")
        return

    if not _SCHEMA_PATH.exists():
        log.warning("validate_against_schema: schema not found at %s — skipping", _SCHEMA_PATH)
        return

    import jsonschema  # noqa: PLC0415 — deferred intentionally

    with open(_SCHEMA_PATH, encoding="utf-8") as f:
        schema = json.load(f)

    validator = jsonschema.Draft7Validator(schema)
    errors = list(validator.iter_errors(data))
    if errors:
        first = errors[0]
        raise ValueError(
            f"dlssconfig schema validation failed: {first.message} at {first.json_path}"
        )

    log.debug("validate_against_schema: OK")


def write_atomic(path: str, data: dict[str, Any]) -> None:
    """Atomically write dict as JSON (write-to-tmp + os.replace)."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)

    tmp_path = str(p) + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    os.replace(tmp_path, str(p))
    log.debug("write_atomic: wrote %s", p)


def write_dlssconfig(
    output_dir: str,
    top_level: dict[str, Any],
    frames: dict[str, Any],
) -> None:
    """Combine top-level + per-frame data, validate, atomically write dlssconfig.json.

    Raises ValueError if schema validation fails (file is NOT written in that case).
    """
    config = dict(top_level)
    config["frames"] = frames

    validate_against_schema(config)

    out_path = Path(output_dir) / "dlssconfig.json"
    write_atomic(str(out_path), config)
    log.info("write_dlssconfig: wrote %s (%d frames)", out_path, len(frames))
