"""
DLSS quality mode constants — MUST match src/core/types.h exactly.
SR §3.7.1.1 scaling ratio table and minimum phase count table.
"""

# Scaling ratios per quality mode (SR §3.7.1.1)
DLSS_RATIO = {
    'DLAA': 1.0,
    'ULTRA_QUALITY': 1.0,
    'QUALITY': 1.5,
    'BALANCED': 1.724,
    'PERFORMANCE': 2.0,
    'ULTRA_PERFORMANCE': 3.0,
}

# Jitter phase counts per quality mode (SR §3.7.1.1 minimum table)
JITTER_PHASES = {
    'DLAA': 8,
    'ULTRA_QUALITY': 8,
    'QUALITY': 18,
    'BALANCED': 24,
    'PERFORMANCE': 32,
    'ULTRA_PERFORMANCE': 72,
}

def get_render_percentage(quality_mode: str) -> int:
    """
    Compute the resolution_percentage Blender must use for this quality mode.
    Uses integer arithmetic to avoid float drift.
    Returns integer percent (e.g., 50 for Performance 2x).
    """
    ratio = DLSS_RATIO[quality_mode]
    return round(100.0 / ratio)

def get_render_resolution(output_width: int, output_height: int, quality_mode: str):
    """
    Compute actual render resolution in pixels (integer arithmetic).
    render_w = output_w * percentage // 100
    """
    pct = get_render_percentage(quality_mode)
    return (output_width * pct // 100, output_height * pct // 100)
