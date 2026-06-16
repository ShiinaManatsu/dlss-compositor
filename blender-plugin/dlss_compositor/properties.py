from __future__ import annotations

import bpy

QUALITY_MODES = [
    ('DLAA', 'DLAA', 'Max quality, 1x (no upscale)', 0),
    ('ULTRA_QUALITY', 'Ultra Quality', '1.3x upscale (77%)', 1),
    ('QUALITY', 'Quality', '1.5x upscale', 2),
    ('BALANCED', 'Balanced', '1.724x upscale', 3),
    ('PERFORMANCE', 'Performance', '2x upscale', 4),
    ('ULTRA_PERFORMANCE', 'Ultra Performance', '3x upscale', 5),
]

class DLSSCompositorProperties(bpy.types.PropertyGroup):
    enabled: bpy.props.BoolProperty(
        name="Enable DLSS Compositor",
        description="Enable transparent resolution override and jitter during render",
        default=True
    )
    quality_mode: bpy.props.EnumProperty(
        name="Quality Mode",
        items=QUALITY_MODES,
        default='QUALITY'
    )
    output_dir: bpy.props.StringProperty(
        name="Output Directory",
        subtype='DIR_PATH',
        default=""
    )
    # NOTE: NO output_width/height (use scene.render.resolution_x/y)
    # NOTE: NO resolution_percentage (handler save/override/restore transparently)
    # NOTE: NO cut_after (camera cut detected by CLI via matrix discontinuity)
    # NOTE: NO scale (derived from quality_mode via DLSS_RATIO)

def register() -> None:
    bpy.utils.register_class(DLSSCompositorProperties)
    bpy.types.Scene.dlss_compositor = bpy.props.PointerProperty(type=DLSSCompositorProperties)

def unregister() -> None:
    if hasattr(bpy.types.Scene, "dlss_compositor"):
        del bpy.types.Scene.dlss_compositor
    bpy.utils.unregister_class(DLSSCompositorProperties)
