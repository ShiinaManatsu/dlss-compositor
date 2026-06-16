from __future__ import annotations

from bpy.types import Context, Scene

def configure_aov_passes(context: Context) -> None:
    """Enable Combined, Depth (Z), Vector passes on the active view layer."""
    view_layer = context.view_layer
    view_layer.use_pass_z = True
    view_layer.use_pass_vector = True
    # Combined is always on by default, but make it explicit
    # view_layer.use_pass_combined = True  # attribute may vary by Blender version

def configure_cycles_denoise(scene: Scene) -> None:
    """Force Cycles denoising ON — DLSS trained on denoised frames."""
    if hasattr(scene, 'cycles'):
        scene.cycles.use_denoising = True
