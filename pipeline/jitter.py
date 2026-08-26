"""Halton(2,3) jitter keyed by Blender frame number.

Copied from blender-plugin/dlss_compositor/jitter.py so inject.py does not
need the addon enabled. Phase counts match DLSS SR §3.7.1.1.
"""
from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import bpy

# Minimum phase count starts at 8; Halton radices stay 2 and 3.
JITTER_PHASES = {
    "DLAA": 8,
    "ULTRA_QUALITY": 14,
    "QUALITY": 18,
    "BALANCED": 24,
    "PERFORMANCE": 32,
    "ULTRA_PERFORMANCE": 72,
}

CLAMP_MAX = 0.499
CLAMP_MIN = -0.499


def halton(index: int, base: int) -> float:
    result = 0.0
    denominator = 1.0
    n = index
    while n > 0:
        denominator *= base
        n, remainder = divmod(n, base)
        result += remainder / denominator
    return result


def _clamp_jitter(value: float) -> float:
    return max(CLAMP_MIN, min(CLAMP_MAX, value))


def jitter_for_frame_number(quality_mode: str, frame_number: int) -> tuple[float, float]:
    """Blender frame 1 maps to Halton phase 1. Jitter is keyed by frame number."""
    phase_count = JITTER_PHASES[quality_mode]
    phase = ((frame_number - 1) % phase_count) + 1
    return (
        _clamp_jitter(halton(phase, 2) - 0.5),
        _clamp_jitter(halton(phase, 3) - 0.5),
    )


def camera_shift_denominator(scene: "bpy.types.Scene") -> float:
    camera = scene.camera
    render = scene.render
    pct = render.resolution_percentage / 100.0
    render_w = render.resolution_x * pct
    render_h = render.resolution_y * pct
    if render_w <= 0 or render_h <= 0:
        return 0.0

    aspect_x = render.pixel_aspect_x
    aspect_y = render.pixel_aspect_y
    sensor_fit = getattr(camera.data, "sensor_fit", "AUTO") if camera is not None else "AUTO"
    if sensor_fit == "AUTO":
        sensor_fit = "HORIZONTAL" if aspect_x * render_w >= aspect_y * render_h else "VERTICAL"

    if sensor_fit == "VERTICAL":
        return (aspect_y / aspect_x) * render_h if aspect_x > 0 else 0.0
    return render_w


def apply_jitter_to_frame(
    scene: "bpy.types.Scene",
    jitter_x: float,
    jitter_y: float,
    base_shift_x: float,
    base_shift_y: float,
) -> None:
    camera = scene.camera
    if camera is None:
        return
    shift_denominator = camera_shift_denominator(scene)
    if shift_denominator <= 0:
        return
    camera.data.shift_x = base_shift_x + (jitter_x / shift_denominator)
    camera.data.shift_y = base_shift_y + (jitter_y / shift_denominator)
