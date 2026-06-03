from __future__ import annotations

import bpy

from .quality_mapping import JITTER_PHASES

# Halton(2,3) jitter sequence for DLSS temporal accumulation.
# Per DLSS SR §3.7.1.1: base=8, phase counts per quality mode.
#
# CRITICAL: The "base=8" in the plan means the minimum phase count starts at 8
# (not that the Halton radix base is 8). The Halton sequence uses radix base 2 and 3.
# The phase_count is the sequence length before repeating:
# - DLAA: 8 phases, UltraQuality: 14 phases
# - Quality: 18 phases
# - Balanced: 24 phases
# - Performance: 32 phases
# - UltraPerformance: 72 phases

CLAMP_MAX = 0.499
CLAMP_MIN = -0.499

def halton(index: int, base: int) -> float:
    """
    Compute the Halton sequence value at index for given base.
    Uses van der Corput construction.
    
    Args:
        index: 1-based sequence index (1, 2, 3, ...)
        base: radix base (2 or 3 for DLSS)
    
    Returns:
        Float in (0, 1) exclusive
        
    Examples:
        halton(1, 2) == 0.5
        halton(2, 2) == 0.25
        halton(3, 2) == 0.75
        halton(1, 3) ≈ 0.3333
        halton(2, 3) ≈ 0.6667
    """
    result = 0.0
    denominator = 1.0
    n = index
    while n > 0:
        denominator *= base
        n, remainder = divmod(n, base)
        result += remainder / denominator
    return result


def _clamp_jitter(value: float) -> float:
    """
    Clamp jitter to [-0.499, +0.499].
    Per DLSS SR §3.7.1.1 jitter pixel space must be in (-0.5, +0.5).
    We use -0.499 to +0.499 to prevent floating-point boundary issues.
    """
    return max(CLAMP_MIN, min(CLAMP_MAX, value))


def generate_jitter_sequence(quality_mode: str, frame_count: int) -> list[tuple[float, float]]:
    """
    Generate per-frame jitter pairs for the given quality mode.
    
    Args:
        quality_mode: one of 'DLAA', 'ULTRA_QUALITY', 'QUALITY', 'BALANCED', 
                      'PERFORMANCE', 'ULTRA_PERFORMANCE'
        frame_count: number of frames to generate jitter for
    
    Returns:
        List of (jitter_x, jitter_y) tuples, each in [-0.499, +0.499]
        
    Note:
        - Frame 0 uses phase index 1 (Halton is 1-indexed)
        - Sequence wraps at phase_count (modulo)
        - jitter_x from Halton base 2, jitter_y from Halton base 3
        - Both offset by -0.5 to center around 0
    """
    phase_count = JITTER_PHASES[quality_mode]
    result = []
    for frame_num in range(frame_count):
        phase = (frame_num % phase_count) + 1  # 1-indexed Halton
        jitter_x = _clamp_jitter(halton(phase, 2) - 0.5)
        jitter_y = _clamp_jitter(halton(phase, 3) - 0.5)
        result.append((jitter_x, jitter_y))
    return result


def apply_jitter_to_frame(scene: bpy.types.Scene, frame_num: int, jitter_x: float, jitter_y: float) -> None:
    """
    Apply sub-pixel jitter to camera for this frame via lens shift.

    jitter_x/y are in pixel space [-0.499, +0.499] relative to the
    RENDER resolution (after resolution_percentage override), not the
    output resolution.  Blender camera shift_x/y of 1.0 equals one full
    render-pixel-width / render-pixel-height shift respectively.
    """
    camera = scene.camera
    if camera is None:
        return

    render = scene.render
    pct = render.resolution_percentage / 100.0

    # Actual rendered pixel dimensions (after DLSS quality override)
    render_w = render.resolution_x * pct
    render_h = render.resolution_y * pct

    base_shift_x = float(scene.get("_dlss_saved_shift_x", camera.data.shift_x))
    base_shift_y = float(scene.get("_dlss_saved_shift_y", camera.data.shift_y))

    if render_w > 0:
        camera.data.shift_x = base_shift_x + (jitter_x / render_w)
    if render_h > 0:
        camera.data.shift_y = base_shift_y + (jitter_y / render_h)
