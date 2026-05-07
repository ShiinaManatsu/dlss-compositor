"""DLSS Compositor — AOV Export Preset for Blender.

Configures render passes and custom AOVs needed by the DLSS Compositor
pipeline. Run headless with:

    blender --background --factory-startup --python aov_export_preset.py -- --test
"""

bl_info = {
    "name": "DLSS Compositor — AOV Export Preset",
    "author": "DLSS Compositor",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "Render Properties > DLSS Compositor",
    "description": "Configure render passes and AOVs for DLSS Compositor workflow",
    "category": "Render",
}

import bpy  # noqa: E402


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


def save_original_shift(scene):
    """Render init handler: save original camera shift values."""
    global _original_shift_x, _original_shift_y, _jitter_enabled, _runtime_jitter
    _runtime_jitter.clear()
    camera = scene.camera
    if camera and camera.data:
        _original_shift_x = camera.data.shift_x
        _original_shift_y = camera.data.shift_y
        _jitter_enabled = True


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
            self.report({"ERROR"}, f"Empty frame range ({frame_start}–{frame_end}).")
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

        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
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

        # 2 — Compositor: multilayer EXR file output ---------------------------
        scene = context.scene
        if bpy.app.version >= (5, 0, 0):
            # Blender 5.x: compositor uses compositing_node_group
            tree = scene.compositing_node_group
            if tree is None:
                tree = bpy.data.node_groups.new("Compositing", "CompositorNodeTree")
                scene.compositing_node_group = tree
        else:
            # Blender 4.x: compositor uses scene.node_tree
            scene.use_nodes = True
            tree = scene.node_tree
        if tree is None:
            self.report({"ERROR"}, "Could not initialise compositor node tree.")
            return {"CANCELLED"}

        # Find or create Render Layers node
        render_layer_node = None
        for node in tree.nodes:
            if node.type == "R_LAYERS":
                render_layer_node = node
                break
        if render_layer_node is None:
            render_layer_node = tree.nodes.new("CompositorNodeRLayers")

        # Find or create File Output node
        file_output = None
        for node in tree.nodes:
            if node.type == "OUTPUT_FILE":
                file_output = node
                break
        if file_output is None:
            file_output = tree.nodes.new("CompositorNodeOutputFile")

        file_output.format.file_format = "OPEN_EXR_MULTILAYER"
        file_output.format.color_depth = "32"
        file_output.format.exr_codec = "ZIP"

        if scene.dlsscomp_output_dir:
            if bpy.app.version >= (5, 0, 0):
                file_output.directory = scene.dlsscomp_output_dir
            else:
                file_output.base_path = scene.dlsscomp_output_dir

        # Connect render layer passes to file output inputs
        # Blender 5.x renamed some render pass outputs
        if bpy.app.version >= (5, 0, 0):
            # Blender 5.x: VALUE→FLOAT
            pass_names = [
                ("Image", "RGBA"),  # Combined
                ("Depth", "FLOAT"),  # Z
                ("Vector", "VECTOR"),
            ]
        else:
            pass_names = [
                ("Image", "RGBA"),
                ("Depth", "VALUE"),
                ("Vector", "VECTOR"),
            ]

        # Get existing slots collection (API changed in Blender 5.x)
        if bpy.app.version >= (5, 0, 0):
            slots_collection = file_output.file_output_items
        else:
            slots_collection = file_output.file_slots
        existing_slots = {s.name for s in slots_collection}

        for pname, sock_type in pass_names:
            # Ensure a matching file slot exists
            if pname not in existing_slots:
                if bpy.app.version >= (5, 0, 0):
                    slots_collection.new(sock_type, pname)
                else:
                    slots_collection.new(pname)

            # Try to connect
            if pname in render_layer_node.outputs:
                rl_out = render_layer_node.outputs[pname]
                fo_in = None
                for inp in file_output.inputs:
                    if inp.name == pname:
                        fo_in = inp
                        break
                if fo_in is not None and not fo_in.is_linked:
                    tree.links.new(rl_out, fo_in)

        # 5 — Report -----------------------------------------------------------
        configured = "Combined, Z, Vector"
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

        layout.prop(context.scene, "dlsscomp_output_dir", text="Output Directory")

        layout.prop(context.scene, "dlsscomp_output_scale", text="Output Scale")

        layout.operator(
            DLSSCOMP_OT_configure_passes.bl_idname,
            text="Configure All Passes",
            icon="RENDERLAYERS",
        )

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
                bpy.path.abspath(context.scene.dlsscomp_output_dir), "camera.json"
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
    bpy.utils.register_class(DLSSCOMP_OT_export_camera)
    bpy.utils.register_class(DLSSCOMP_OT_configure_passes)
    bpy.utils.register_class(DLSSCOMP_OT_cleanup_legacy_aov_nodes)
    bpy.utils.register_class(DLSSCOMP_PT_export_panel)

    # Register jitter handlers
    bpy.app.handlers.render_init.append(save_original_shift)
    bpy.app.handlers.render_pre.append(apply_jitter)
    bpy.app.handlers.render_complete.append(restore_shift)
    bpy.app.handlers.render_cancel.append(restore_shift)


def unregister():
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
            args = sys.argv[sys.argv.index("--") + 1 :]

        register()

        if "--test" in args:
            bpy.ops.dlsscomp.configure_passes()
            print("Passes configured: Combined, Z, Vector")
            sys.exit(0)
    except Exception as e:
        import sys as _sys

        print(f"ERROR: {e}", file=_sys.stderr)
        sys.exit(1)
