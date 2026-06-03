"""
DLSS Compositor dlssconfig.json writer — Blender side.
Schema single source of truth: src/core/dlssconfig.schema.json
This module builds, validates, and atomically writes dlssconfig.json.
"""
from __future__ import annotations

import os
import json
import logging
from pathlib import Path
from typing import Any, Dict

from bpy.types import Scene

log = logging.getLogger(__name__)

# HDR display devices per Blender 5.1 docs
# https://docs.blender.org/manual/en/latest/render/color_management/displays_views.html
HDR_DISPLAY_DEVICES = {'Rec.2100-PQ', 'Rec.2100-HLG'}

# Path to schema relative to repo root — single source of truth
_SCHEMA_PATH = Path(__file__).parent.parent.parent / 'src' / 'core' / 'dlssconfig.schema.json'

JITTER_CLAMP = 0.499

def _clamp_jitter(value: float, warn_name: str = '') -> float:
    """Clamp jitter to [-0.499, +0.499]. NEVER reject; always clamp + warn if needed."""
    if value < -0.499 or value > 0.499:
        if warn_name:
            log.warning(f"DLSS: jitter {warn_name}={value:.6f} out of range, clamping to ±0.499")
    return max(-JITTER_CLAMP, min(JITTER_CLAMP, value))


def build_top_level_config(scene: Scene, props: object) -> Dict[str, Any]:
    """
    Build the top-level dlssconfig dict from Blender scene + DLSS properties.

    Does NOT include 'frames' key — that is merged separately via merge_per_frame_jsons().
    """
    from .quality_mapping import JITTER_PHASES

    output_w = scene.render.resolution_x
    output_h = scene.render.resolution_y
    pct = scene.render.resolution_percentage  # Already overridden by handler at this point

    render_w = int(output_w * pct / 100)
    render_h = int(output_h * pct / 100)

    display_device = getattr(scene.display_settings, 'display_device', 'sRGB')
    view_transform = getattr(scene.view_settings, 'view_transform', 'Filmic')

    # is_hdr derived from display_device — NOT user-configurable
    is_hdr = display_device in HDR_DISPLAY_DEVICES

    quality_mode = props.quality_mode.lower()  # e.g., 'performance'
    jitter_phases = JITTER_PHASES[props.quality_mode]

    return {
        'version': 1,
        'output_resolution': {'width': output_w, 'height': output_h},
        'render_resolution': {'width': render_w, 'height': render_h},
        'quality_mode': quality_mode,
        'display_device': display_device,
        'view_transform': view_transform,
        'is_hdr': is_hdr,
        'fps': scene.render.fps,
        'filename_pattern': r'^\d{4}\.exr$',
        'jitter_phases': jitter_phases,
    }


def merge_per_frame_jsons(output_dir: str) -> Dict[str, Any]:
    """
    Scan output_dir/frames/*.cam.json and merge into a 'frames' dict.

    Returns dict mapping zero-padded frame number string → frame data dict.
    Files are sorted by frame number.
    """
    frames_dir = Path(output_dir) / 'frames'
    frames = {}

    if not frames_dir.exists():
        return frames

    cam_files = sorted(frames_dir.glob('*.cam.json'))
    for cam_file in cam_files:
        try:
            with open(cam_file) as f:
                frame_data = json.load(f)

            # Extract frame number from filename (e.g., "0042.cam.json" → "0042")
            frame_key = cam_file.stem.replace('.cam', '')

            # Clamp jitter values in frame data
            if 'jitter_x' in frame_data:
                frame_data['jitter_x'] = _clamp_jitter(frame_data['jitter_x'], 'jitter_x')
            if 'jitter_y' in frame_data:
                frame_data['jitter_y'] = _clamp_jitter(frame_data['jitter_y'], 'jitter_y')

            frames[frame_key] = frame_data
        except (json.JSONDecodeError, KeyError) as e:
            log.warning(f"DLSS: Failed to read {cam_file}: {e}")

    return frames


def validate_against_schema(data: Dict[str, Any]) -> None:
    """
    Validate dlssconfig dict against src/core/dlssconfig.schema.json.

    Raises ValueError with JSON pointer if validation fails.
    Note: Uses jsonschema library (must be installed: pip install jsonschema).
    """
    try:
        import jsonschema
    except ImportError:
        log.warning("DLSS: jsonschema not installed — skipping schema validation")
        return

    if not _SCHEMA_PATH.exists():
        log.warning(f"DLSS: Schema not found at {_SCHEMA_PATH} — skipping validation")
        return

    with open(_SCHEMA_PATH) as f:
        schema = json.load(f)

    try:
        jsonschema.validate(data, schema)
    except jsonschema.ValidationError as e:
        raise ValueError(f"dlssconfig schema validation failed: {e.message} at {e.json_path}")


def write_atomic(path: str, data: Dict[str, Any]) -> None:
    """
    Atomically write dict as JSON to path.
    Uses write-to-temp + os.replace() pattern to prevent partial files.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    tmp_path = str(path) + '.tmp'
    try:
        with open(tmp_path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        os.replace(tmp_path, str(path))
    except Exception:
        # Clean up temp file if write failed
        if os.path.exists(tmp_path):
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
        raise


def write_dlssconfig(output_dir: str, top_level: Dict[str, Any], frames: Dict[str, Any]) -> None:
    """
    Combine top-level config + per-frame data, validate, and atomically write dlssconfig.json.

    Args:
        output_dir: directory where dlssconfig.json will be written
        top_level: dict from build_top_level_config()
        frames: dict from merge_per_frame_jsons()

    Raises:
        ValueError: if schema validation fails (file is NOT written)
    """
    config = dict(top_level)
    config['frames'] = frames

    # Validate before writing — prevent writing invalid config
    validate_against_schema(config)

    out_path = Path(output_dir) / 'dlssconfig.json'
    write_atomic(str(out_path), config)
    log.info(f"DLSS: dlssconfig.json written to {out_path} ({len(frames)} frames)")
