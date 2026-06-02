import bpy

def configure_aov_passes(context):
    """Enable Combined, Depth (Z), Vector passes on the active view layer."""
    view_layer = context.view_layer
    view_layer.use_pass_z = True
    view_layer.use_pass_vector = True
    # Combined is always on by default, but make it explicit
    # view_layer.use_pass_combined = True  # attribute may vary by Blender version

def configure_cycles_denoise(scene):
    """Force Cycles denoising ON — DLSS trained on denoised frames."""
    if hasattr(scene, 'cycles'):
        scene.cycles.use_denoising = True
