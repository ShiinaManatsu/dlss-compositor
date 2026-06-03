"""DLSS Compositor dlssconfig.json writer — Blender side."""
from __future__ import annotations

import json
import logging
import os
from pathlib import Path
from typing import Any

from bpy.types import Scene

log = logging.getLogger("dlss_compositor.dlssconfig_writer")

# HDR display devices per Blender 5.1 docs
HDR_DISPLAY_DEVICES = {"Rec.2100-PQ", "Rec.2100-HLG"}

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


def merge_per_frame_jsons(output_dir: str, frame_numbers: list[int] | None = None) -> dict[str, Any]:
    """Merge continuous per-frame cam.json files into a frames dict."""
    frames_dir = Path(output_dir) / "frames"
    frames: dict[str, Any] = {}

    if not frames_dir.exists():
        log.debug("merge_per_frame_jsons: frames dir does not exist: %s", frames_dir)
        return frames

    if frame_numbers is None:
        cam_files = sorted(frames_dir.glob("*.cam.json"))
        frame_numbers = []
        for cam_file in cam_files:
            frame_key = cam_file.stem.replace(".cam", "")
            if frame_key.isdigit():
                frame_numbers.append(int(frame_key))
    else:
        frame_numbers = sorted(set(frame_numbers))
        cam_files = [frames_dir / f"{frame_num:04d}.cam.json" for frame_num in frame_numbers]

    log.debug("merge_per_frame_jsons: found %d cam.json files", len(cam_files))

    if not frame_numbers:
        return frames

    continuous_numbers = [frame_numbers[0]]
    previous = frame_numbers[0]
    for frame_num in frame_numbers[1:]:
        if frame_num != previous + 1:
            log.warning(
                "merge_per_frame_jsons: frame gap after %04d; discarding frame %04d and later",
                previous, frame_num,
            )
            break
        continuous_numbers.append(frame_num)
        previous = frame_num

    for frame_num in continuous_numbers:
        cam_file = frames_dir / f"{frame_num:04d}.cam.json"
        with open(cam_file, encoding="utf-8") as f:
            frame_data: dict[str, Any] = json.load(f)

        frame_key = f"{frame_num:04d}"

        if "jitter_x" in frame_data:
            frame_data["jitter_x"] = _clamp_jitter(frame_data["jitter_x"], "jitter_x")
        if "jitter_y" in frame_data:
            frame_data["jitter_y"] = _clamp_jitter(frame_data["jitter_y"], "jitter_y")

        frames[frame_key] = frame_data
        log.debug("merge_per_frame_jsons: loaded frame %s", frame_key)

    return frames


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
    """Combine top-level + per-frame data and atomically write dlssconfig.json."""
    config = dict(top_level)
    config["frames"] = frames

    out_path = Path(output_dir) / "dlssconfig.json"
    write_atomic(str(out_path), config)
    log.info("write_dlssconfig: wrote %s (%d frames)", out_path, len(frames))
