"""DLSS Compositor — AOV Export Preset for Blender.

Configures render passes and custom AOVs needed by the DLSS Compositor
pipeline. Run headless with:

    blender --background --factory-startup --python aov_export_preset.py -- --test
"""

# pyright: reportAttributeAccessIssue=false, reportOptionalMemberAccess=false, reportOptionalSubscript=false, reportArgumentType=false, reportIncompatibleMethodOverride=false

import bpy  # noqa: E402

bl_info = {
    "name": "DLSS Compositor - AOV Export Preset",
    "author": "DLSS Compositor",
    "version": (0, 2, 0),
    "blender": (4, 2, 0),
    "location": "Render Properties > DLSS Compositor",
    "description": "Configure render passes and AOVs for DLSS Compositor workflow",
    "category": "Render",
}


# ---------------------------------------------------------------------------
# Halton Sequence Generator (for DLSS jitter)
# ---------------------------------------------------------------------------


def halton(index, base):
    """Generate Halton sequence value for the given index and base.

    Args:
        index: 0-based sequence index
        base: Prime base (2 for X, 3 for Y)

    Returns:
        Float in [0, 1) range
    """
    result = 0.0
    f = 1.0 / base
    i = index
    while i > 0:
        result += f * (i % base)
        i //= base
        f /= base
    return result


# ---------------------------------------------------------------------------
# Jitter Handler State
# ---------------------------------------------------------------------------

# Global state for camera jitter handlers
_original_shift_x = 0.0
_original_shift_y = 0.0
_jitter_enabled = False

# Global state for render-scale handlers (snapshot of resolution before scaling)
_original_resolution_x = 0
_original_resolution_y = 0


@bpy.app.handlers.persistent
def apply_render_scale(scene):
    """Render init handler: scale render resolution by dlss_compositor_render_scale."""
    global _original_resolution_x, _original_resolution_y
    scale = getattr(scene, 'dlss_compositor_render_scale', 1.0)
    if abs(scale - 1.0) > 0.001:
        _original_resolution_x = scene.render.resolution_x
        _original_resolution_y = scene.render.resolution_y
        scene.render.resolution_x = int(round(_original_resolution_x * scale))
        scene.render.resolution_y = int(round(_original_resolution_y * scale))


@bpy.app.handlers.persistent
def restore_render_scale(scene):
    """Render complete/cancel handler: restore original render resolution."""
    global _original_resolution_x, _original_resolution_y
    if _original_resolution_x > 0:
        scene.render.resolution_x = _original_resolution_x
        scene.render.resolution_y = _original_resolution_y
        _original_resolution_x = 0
        _original_resolution_y = 0


@bpy.app.handlers.persistent
def save_original_shift(scene):
    """Render init handler: save original camera shift values."""
    global _original_shift_x, _original_shift_y, _jitter_enabled, _runtime_jitter
    _runtime_jitter.clear()
    camera = scene.camera
    if camera and camera.data:
        _original_shift_x = camera.data.shift_x
        _original_shift_y = camera.data.shift_y
        _jitter_enabled = True


@bpy.app.handlers.persistent
def apply_jitter(scene):
    """Frame change handler: apply Halton jitter via camera shift."""
    global _jitter_enabled, _original_shift_x, _original_shift_y
    if not _jitter_enabled:
        return

    camera = scene.camera
    if not camera or not camera.data:
        return

    render = scene.render
    frame_num = scene.frame_current

    # Dynamic jitter phases: 8 × output_scale² per NVIDIA spec
    scale = getattr(scene, 'dlsscomp_output_scale', 2.0)
    jitter_phases = max(1, int(round(8.0 * scale * scale)))
    jitter_x = halton(frame_num % jitter_phases, 2) - 0.5
    jitter_y = halton(frame_num % jitter_phases, 3) - 0.5

    # Store applied jitter so export_camera can read it back
    _runtime_jitter[frame_num] = (jitter_x, jitter_y)

    # Convert pixel-space jitter to camera shift units (sensor width units)
    camera.data.shift_x = _original_shift_x + jitter_x / render.resolution_x
    camera.data.shift_y = _original_shift_y + jitter_y / render.resolution_y


@bpy.app.handlers.persistent
def restore_shift(scene):
    """Render complete/cancel handler: restore original camera shift."""
    global _original_shift_x, _original_shift_y, _jitter_enabled
    if not _jitter_enabled:
        return

    camera = scene.camera
    if camera and camera.data:
        camera.data.shift_x = _original_shift_x
        camera.data.shift_y = _original_shift_y

    _jitter_enabled = False


# Per-frame jitter values actually applied by apply_jitter(), keyed by frame number.
# export_camera reads from this dict to guarantee consistency.
_runtime_jitter: dict = {}


# ---------------------------------------------------------------------------
# Operator
# ---------------------------------------------------------------------------


class DLSSCOMP_OT_export_camera(bpy.types.Operator):
    """Export per-frame camera matrices to camera.json alongside the output directory."""

    bl_idname = "dlsscomp.export_camera"
    bl_label = "Export Camera Data"
    bl_options = {"REGISTER"}

    def execute(self, context):
        import json
        import os

        scene = context.scene

        out_dir = scene.dlsscomp_output_dir
        if not out_dir:
            self.report({"ERROR"}, "Set Output Directory first.")
            return {"CANCELLED"}

        output_path = os.path.join(bpy.path.abspath(out_dir), "camera.json")

        cam_obj = scene.camera
        if cam_obj is None:
            self.report({"ERROR"}, "No active camera in the scene.")
            return {"CANCELLED"}

        cam_data = getattr(cam_obj, "data", None)
        fov = getattr(cam_data, "angle", None)
        near_clip = getattr(cam_data, "clip_start", None)
        far_clip = getattr(cam_data, "clip_end", None)
        if None in (fov, near_clip, far_clip):
            self.report({"ERROR"}, "Active camera is missing required data.")
            return {"CANCELLED"}

        render = scene.render
        render_width = render.resolution_x
        render_height = render.resolution_y
        frame_start = scene.frame_start
        frame_end = scene.frame_end

        if frame_end < frame_start:
            self.report(
                {"ERROR"}, f"Empty frame range ({frame_start}–{frame_end}).")
            return {"CANCELLED"}

        scale = getattr(scene, 'dlsscomp_output_scale', 2.0)
        jitter_phases = max(1, int(round(8.0 * scale * scale)))

        data = {
            "version": 1,
            "render_width": render_width,
            "render_height": render_height,
            "jitter_phases": jitter_phases,
            "frames": {},
        }

        depsgraph = context.evaluated_depsgraph_get()

        for frame_num in range(frame_start, frame_end + 1):
            scene.frame_set(frame_num)
            depsgraph.update()
            key = f"{frame_num:04d}"
            matrix_world = [list(row) for row in cam_obj.matrix_world]
            projection = [
                list(row)
                for row in cam_obj.calc_matrix_camera(
                    depsgraph, x=render_width, y=render_height
                )
            ]

            # Prefer the value actually applied by apply_jitter; fall back if not rendered
            if frame_num in _runtime_jitter:
                jitter_x, jitter_y = _runtime_jitter[frame_num]
            else:
                # Fallback: compute from Halton (used when exporting without rendering first)
                jitter_x = halton(frame_num % jitter_phases, 2) - 0.5
                jitter_y = halton(frame_num % jitter_phases, 3) - 0.5

            data["frames"][key] = {
                "matrix_world": matrix_world,
                "projection": projection,
                "fov": fov,
                "aspect_ratio": render_width / render_height,
                "near_clip": near_clip,
                "far_clip": far_clip,
                "jitter_x": jitter_x,
                "jitter_y": jitter_y,
            }

        os.makedirs(os.path.dirname(
            os.path.abspath(output_path)), exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)

        n = len(data["frames"])
        self.report({"INFO"}, f"Exported {n} frame(s) → {output_path}")
        return {"FINISHED"}


class DLSSCOMP_OT_configure_passes(bpy.types.Operator):
    """Enable all render passes and AOVs required by DLSS Compositor."""

    bl_idname = "dlsscomp.configure_passes"
    bl_label = "Configure DLSS Compositor Passes"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        vl = context.view_layer

        # 1 — Built-in render passes -------------------------------------------
        vl.use_pass_combined = True
        vl.use_pass_z = True
        vl.use_pass_vector = True

        # 2 — Compositor: dual standard-EXR file outputs -----------------------
        scene = context.scene
        tree = scene.compositing_node_group
        if tree is None:
            tree = bpy.data.node_groups.new(
                "Compositing", "CompositorNodeTree")
            scene.compositing_node_group = tree
        if tree is None:
            self.report(
                {"ERROR"}, "Could not initialise compositor node tree.")
            return {"CANCELLED"}

        # Find or create Render Layers node
        render_layer_node = None
        for node in tree.nodes:
            if node.type == "R_LAYERS":
                render_layer_node = node
                break
        if render_layer_node is None:
            render_layer_node = tree.nodes.new("CompositorNodeRLayers")

        # Compression codec from Scene property (default ZIP)
        compression_codec = getattr(
            scene, 'dlss_compositor_compression', 'ZIP')

        base_dir = scene.dlsscomp_output_dir or ""

        # --- Node 1: Main EXR (Combined RGBA + Depth Z) -------------------
        # Two slots in OPEN_EXR (non-multilayer) → two sibling files per frame:
        #   <base>/####.exr        (Combined RGBA from Image slot)
        #   <base>/####.depth.exr  (single-channel Z from Depth slot)
        main_out = None
        for node in tree.nodes:
            if node.type == "OUTPUT_FILE" and node.label == 'DLSS_Main':
                main_out = node
                break
        if main_out is None:
            main_out = tree.nodes.new("CompositorNodeOutputFile")
            main_out.label = 'DLSS_Main'
            main_out.name = 'DLSS_Main'

        main_out.format.file_format = "OPEN_EXR"
        main_out.format.color_depth = "32"
        main_out.format.exr_codec = compression_codec
        if base_dir:
            main_out.base_path = base_dir

        # Configure slots: Image (Combined RGBA) + Depth (Z)
        # In OPEN_EXR mode each slot writes its own file using slot.path as prefix.
        main_slots = main_out.file_slots
        existing_main_paths = {s.path for s in main_slots}
        if 'Image' not in {s.name for s in main_slots} and \
                "####" not in existing_main_paths:
            main_slots.new('Image')
        if 'Depth' not in {s.name for s in main_slots} and \
                "####.depth" not in existing_main_paths:
            main_slots.new('Depth')

        # Set slot filename patterns for sibling layout
        for slot in main_slots:
            if slot.name == 'Image' or slot.path == 'Image':
                slot.path = "####"
            elif slot.name == 'Depth' or slot.path == 'Depth':
                slot.path = "####.depth"

        # Connect Render Layers → main_out
        if 'Image' in render_layer_node.outputs:
            for inp in main_out.inputs:
                if inp.name == 'Image' and not inp.is_linked:
                    tree.links.new(render_layer_node.outputs['Image'], inp)
                    break
        if 'Depth' in render_layer_node.outputs:
            for inp in main_out.inputs:
                if inp.name == 'Depth' and not inp.is_linked:
                    tree.links.new(render_layer_node.outputs['Depth'], inp)
                    break

        # --- Node 2: MV EXR (Vector X→R, Y→G, sibling .mv.exr) ------------
        # Vector pass is split via Separate XYZ then re-packed via Combine
        # Color into a single RGBA image so we get a single sibling file:
        #   <base>/####.mv.exr  (R = MV X, G = MV Y, B = 0, A = 1)
        mv_out = None
        for node in tree.nodes:
            if node.type == "OUTPUT_FILE" and node.label == 'DLSS_MV':
                mv_out = node
                break
        if mv_out is None:
            mv_out = tree.nodes.new("CompositorNodeOutputFile")
            mv_out.label = 'DLSS_MV'
            mv_out.name = 'DLSS_MV'

        mv_out.format.file_format = "OPEN_EXR"
        mv_out.format.color_depth = "32"
        # Motion vectors are precision-critical: always lossless ZIP
        mv_out.format.exr_codec = "ZIP"
        if base_dir:
            mv_out.base_path = base_dir

        # Find or create the Separate XYZ node that splits the Vector pass
        separate_xyz = None
        for node in tree.nodes:
            if node.type == "SEPARATE_XYZ" and node.label == 'DLSS_VectorSplit':
                separate_xyz = node
                break
        if separate_xyz is None:
            separate_xyz = tree.nodes.new("CompositorNodeSeparateXYZ")
            separate_xyz.label = 'DLSS_VectorSplit'
            separate_xyz.name = 'DLSS_VectorSplit'

        if 'Vector' in render_layer_node.outputs:
            sep_in = separate_xyz.inputs[0]
            if not sep_in.is_linked:
                tree.links.new(render_layer_node.outputs['Vector'], sep_in)

        # Find or create a Combine Color node to pack X→R, Y→G into RGBA
        combine_color = None
        for node in tree.nodes:
            if node.type == "COMBINE_COLOR" and node.label == 'DLSS_VectorPack':
                combine_color = node
                break
        if combine_color is None:
            combine_color = tree.nodes.new("CompositorNodeCombineColor")
            combine_color.label = 'DLSS_VectorPack'
            combine_color.name = 'DLSS_VectorPack'
            # Default mode is RGB; set explicitly for safety across versions
            try:
                combine_color.mode = 'RGB'
            except (AttributeError, TypeError):
                pass

        # Combine Color inputs: Red (0), Green (1), Blue (2), Alpha (3)
        if len(combine_color.inputs) >= 3:
            red_in = combine_color.inputs[0]
            green_in = combine_color.inputs[1]
            blue_in = combine_color.inputs[2]
            if not red_in.is_linked and 'X' in separate_xyz.outputs:
                tree.links.new(separate_xyz.outputs['X'], red_in)
            if not green_in.is_linked and 'Y' in separate_xyz.outputs:
                tree.links.new(separate_xyz.outputs['Y'], green_in)
            # Force B = 0 (no z-component); leave alpha to default (1.0)
            try:
                blue_in.default_value = 0.0
            except (AttributeError, TypeError):
                pass

        # MV file output: single Image slot, named "####.mv" → ####.mv.exr
        mv_slots = mv_out.file_slots
        # Remove any leftover slots from previous runs (R/G/etc.) that aren't ours
        # Keep an existing Image-ish slot if present
        if len(mv_slots) == 0:
            mv_slots.new('Image')
        # Set first slot's path to produce the sibling filename
        mv_slots[0].path = "####.mv"

        # Connect Combine Color → MV Image slot
        if len(mv_out.inputs) > 0:
            mv_in = mv_out.inputs[0]
            if not mv_in.is_linked and 'Image' in combine_color.outputs:
                tree.links.new(combine_color.outputs['Image'], mv_in)

        # 5 — Report -----------------------------------------------------------
        configured = "Combined, Z, Vector (dual EXR sibling files)"
        self.report(
            {"INFO"},
            f"Passes configured: {configured}",
        )

        return {"FINISHED"}


class DLSSCOMP_OT_cleanup_legacy_aov_nodes(bpy.types.Operator):
    """Remove legacy DLSS AOV Output nodes from all materials in the scene."""

    bl_idname = "dlsscomp.cleanup_legacy_aov_nodes"
    bl_label = "Remove DLSS AOV Output Nodes"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        removed = 0
        for mat in bpy.data.materials:
            if mat.node_tree is None:
                continue
            nodes_to_remove = [
                node for node in mat.node_tree.nodes
                if node.type == "OUTPUT_AOV" and node.name in ("Roughness",)
            ]
            for node in nodes_to_remove:
                mat.node_tree.nodes.remove(node)
                removed += 1
        self.report({"INFO"}, f"Removed {removed} legacy AOV Output node(s)")
        return {"FINISHED"}


# ---------------------------------------------------------------------------
# Panel
# ---------------------------------------------------------------------------


class DLSSCOMP_PT_export_panel(bpy.types.Panel):
    """DLSS Compositor export settings panel."""

    bl_label = "DLSS Compositor"
    bl_idname = "DLSSCOMP_PT_export_panel"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"

    def draw(self, context):
        layout = self.layout
        vl = context.view_layer

        layout.prop(context.scene, "dlsscomp_output_dir",
                    text="Output Directory")

        layout.prop(context.scene, "dlsscomp_output_scale", text="Output Scale")

        layout.prop(context.scene, "dlss_compositor_compression",
                    text="EXR Compression")
        layout.prop(context.scene, "dlss_compositor_render_scale",
                    text="Render Scale", slider=True)

        layout.operator(
            DLSSCOMP_OT_configure_passes.bl_idname,
            text="Configure All Passes",
            icon="RENDERLAYERS",
        )
        layout.label(text="Standard EXR (opens in RenderDoc)", icon="INFO")

        layout.operator(
            DLSSCOMP_OT_cleanup_legacy_aov_nodes.bl_idname,
            text="Remove Legacy AOV Nodes",
            icon="TRASH",
        )

        layout.separator()
        layout.operator(
            DLSSCOMP_OT_export_camera.bl_idname,
            text="Export Camera Data",
            icon="CAMERA_DATA",
        )
        if context.scene.dlsscomp_output_dir:
            import os

            path = os.path.join(
                bpy.path.abspath(
                    context.scene.dlsscomp_output_dir), "camera.json"
            )
            layout.label(text=f"→ {path}", icon="FILE_TICK")

        layout.separator()
        layout.label(text="Pass Status:", icon="INFO")

        col = layout.column(align=True)
        col.label(
            text=f"Combined: {'ON' if vl.use_pass_combined else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_combined else "X",
        )
        col.label(
            text=f"Z (Depth): {'ON' if vl.use_pass_z else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_z else "X",
        )
        col.label(
            text=f"Vector: {'ON' if vl.use_pass_vector else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_vector else "X",
        )


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------


def register():
    bpy.types.Scene.dlsscomp_output_dir = bpy.props.StringProperty(
        name="Output Directory", subtype="DIR_PATH", default=""
    )
    bpy.types.Scene.dlsscomp_output_scale = bpy.props.FloatProperty(
        name="Output Scale",
        description="DLSS upscaling factor (1.0–4.0). Controls jitter phase count.",
        default=2.0,
        min=1.0,
        max=4.0,
    )
    bpy.types.Scene.dlss_compositor_compression = bpy.props.EnumProperty(
        name="EXR Compression",
        description="Compression codec for exported EXR files",
        items=[
            ('NONE', 'None', 'No compression'),
            ('ZIP', 'ZIP (lossless)', 'ZIP lossless compression'),
            ('ZIPS', 'ZIPS (lossless)', 'ZIPS scanline lossless compression'),
            ('PIZ', 'PIZ (lossless)', 'PIZ wavelet lossless compression'),
            ('DWAA', 'DWAA (lossy)', 'DWAA lossy compression'),
            ('DWAB', 'DWAB (lossy)', 'DWAB lossy compression'),
        ],
        default='ZIP',
    )
    bpy.types.Scene.dlss_compositor_render_scale = bpy.props.FloatProperty(
        name="Render Scale",
        description="Render at this fraction of the configured resolution "
                    "(DLSS upscale input scale)",
        default=1.0,
        min=0.1,
        max=2.0,
        step=5,
        precision=2,
        subtype='FACTOR',
    )
    bpy.utils.register_class(DLSSCOMP_OT_export_camera)
    bpy.utils.register_class(DLSSCOMP_OT_configure_passes)
    bpy.utils.register_class(DLSSCOMP_OT_cleanup_legacy_aov_nodes)
    bpy.utils.register_class(DLSSCOMP_PT_export_panel)

    # Register jitter handlers
    bpy.app.handlers.render_init.append(save_original_shift)
    bpy.app.handlers.render_pre.append(apply_jitter)
    bpy.app.handlers.render_complete.append(restore_shift)
    bpy.app.handlers.render_cancel.append(restore_shift)

    # Register render-scale handlers
    bpy.app.handlers.render_init.append(apply_render_scale)
    bpy.app.handlers.render_complete.append(restore_render_scale)
    bpy.app.handlers.render_cancel.append(restore_render_scale)


def unregister():
    # Unregister render-scale handlers
    if apply_render_scale in bpy.app.handlers.render_init:
        bpy.app.handlers.render_init.remove(apply_render_scale)
    if restore_render_scale in bpy.app.handlers.render_complete:
        bpy.app.handlers.render_complete.remove(restore_render_scale)
    if restore_render_scale in bpy.app.handlers.render_cancel:
        bpy.app.handlers.render_cancel.remove(restore_render_scale)

    # Unregister jitter handlers
    if save_original_shift in bpy.app.handlers.render_init:
        bpy.app.handlers.render_init.remove(save_original_shift)
    if apply_jitter in bpy.app.handlers.render_pre:
        bpy.app.handlers.render_pre.remove(apply_jitter)
    if restore_shift in bpy.app.handlers.render_complete:
        bpy.app.handlers.render_complete.remove(restore_shift)
    if restore_shift in bpy.app.handlers.render_cancel:
        bpy.app.handlers.render_cancel.remove(restore_shift)

    bpy.utils.unregister_class(DLSSCOMP_PT_export_panel)
    bpy.utils.unregister_class(DLSSCOMP_OT_cleanup_legacy_aov_nodes)
    bpy.utils.unregister_class(DLSSCOMP_OT_configure_passes)
    bpy.utils.unregister_class(DLSSCOMP_OT_export_camera)
    del bpy.types.Scene.dlss_compositor_render_scale
    del bpy.types.Scene.dlss_compositor_compression
    del bpy.types.Scene.dlsscomp_output_dir
    del bpy.types.Scene.dlsscomp_output_scale


# ---------------------------------------------------------------------------
# Headless / script-mode entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    try:
        args = []
        if "--" in sys.argv:
            args = sys.argv[sys.argv.index("--") + 1:]

        register()

        if "--test" in args:
            bpy.ops.dlsscomp.configure_passes()
            print("Passes configured: Combined, Z, Vector")
            sys.exit(0)
    except Exception as e:
        import sys as _sys

        print(f"ERROR: {e}", file=_sys.stderr)
        sys.exit(1)
