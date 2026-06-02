"""
Halton(2,3) jitter sequence for DLSS temporal accumulation.
Per DLSS SR §3.7.1.1: base=8, phase counts per quality mode.

CRITICAL: The "base=8" in the plan means the minimum phase count starts at 8 
(not that the Halton radix base is 8). The Halton sequence uses radix base 2 and 3.
The phase_count is the sequence length before repeating:
- DLAA/UltraQuality: 8 phases
- Quality: 18 phases  
- Balanced: 24 phases
- Performance: 32 phases
- UltraPerformance: 72 phases
"""
from .quality_mapping import JITTER_PHASES

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


def generate_jitter_sequence(quality_mode: str, frame_count: int) -> list:
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


def apply_jitter_to_frame(scene, frame_num: int, jitter_x: float, jitter_y: float):
    """
    Apply sub-pixel jitter to camera for this frame.
    Uses camera shift (lens shift) which is a stable Blender API.
    
    NOTE: The jitter values are stored in dlssconfig.json per-frame;
    this function applies them to the Blender camera during render.
    Must be called in render_pre handler before each frame renders.
    """
    camera = scene.camera
    if camera is None:
        return
    
    # Store original shift to restore later
    # (Handler T14 manages save/restore lifecycle; this just applies the offset)
    # Convert pixel jitter to normalized camera shift
    # jitter_x/y are in pixel space [-0.499, +0.499]
    # Blender camera shift is in normalized sensor units
    # For a typical sensor width, 1 pixel ≈ 1/render_width sensor units
    render = scene.render
    width = render.resolution_x
    height = render.resolution_y
    
    if width > 0:
        camera.data.shift_x = (jitter_x / width) * (height / width)
    if height > 0:
        camera.data.shift_y = jitter_y / height
