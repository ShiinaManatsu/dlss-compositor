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
# Operator
# ---------------------------------------------------------------------------


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
        vl.use_pass_normal = True
        vl.use_pass_diffuse_color = True
        vl.use_pass_glossy_color = True

        # 2 — Custom AOV: Roughness -------------------------------------------
        existing_aov_names = {aov.name for aov in vl.aovs}
        if "Roughness" not in existing_aov_names:
            aov = vl.aovs.add()
            aov.name = "Roughness"
            aov.type = "VALUE"

        # 3 — Wire Principled BSDF → AOV Output per material ------------------
        for mat in bpy.data.materials:
            if mat.node_tree is None:
                continue
            nodes = mat.node_tree.nodes
            links = mat.node_tree.links

            # Find the Principled BSDF node
            principled = None
            for node in nodes:
                if node.type == "BSDF_PRINCIPLED":
                    principled = node
                    break
            if principled is None:
                continue

            # Skip if an AOV Output for "Roughness" already exists
            has_aov_output = any(
                n.type == "OUTPUT_AOV" and n.name == "Roughness" for n in nodes
            )
            if has_aov_output:
                continue

            # Add AOV Output node and connect
            aov_node = nodes.new("ShaderNodeOutputAOV")
            aov_node.name = "Roughness"
            aov_node.aov_name = "Roughness"

            # Find the "Roughness" output socket on Principled BSDF
            rough_out = None
            for out in principled.outputs:
                if out.name == "Roughness":
                    rough_out = out
                    break

            if rough_out is not None:
                # Connect to the "Value" input on the AOV Output node
                value_in = None
                for inp in aov_node.inputs:
                    if inp.name == "Value":
                        value_in = inp
                        break
                if value_in is not None:
                    links.new(rough_out, value_in)

        # 4 — Compositor: multilayer EXR file output ---------------------------
        scene = context.scene
        scene.use_nodes = True
        tree = scene.node_tree

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
            file_output.base_path = scene.dlsscomp_output_dir

        # Connect render layer passes to file output inputs
        pass_names = [
            "Image",  # Combined
            "Depth",  # Z
            "Normal",
            "Vector",
            "DiffCol",
            "GlossCol",
            "Roughness",
        ]

        # Remove default slots beyond what we need
        existing_slots = {s.name for s in file_output.file_slots}

        for pname in pass_names:
            # Ensure a matching file slot exists
            if pname not in existing_slots:
                file_output.file_slots.new(pname)

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
        configured = "Combined, Z, Vector, Normal, DiffCol, GlossCol, Roughness(AOV)"
        self.report({"INFO"}, f"Passes configured: {configured}")

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

        layout.operator(
            DLSSCOMP_OT_configure_passes.bl_idname,
            text="Configure All Passes",
            icon="RENDERLAYERS",
        )

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
        col.label(
            text=f"Normal: {'ON' if vl.use_pass_normal else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_normal else "X",
        )
        col.label(
            text=f"Diffuse Color: {'ON' if vl.use_pass_diffuse_color else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_diffuse_color else "X",
        )
        col.label(
            text=f"Glossy Color: {'ON' if vl.use_pass_glossy_color else 'OFF'}",
            icon="CHECKMARK" if vl.use_pass_glossy_color else "X",
        )

        # Roughness AOV status
        aov_names = {aov.name for aov in vl.aovs}
        col.label(
            text=f"Roughness AOV: {'ON' if 'Roughness' in aov_names else 'OFF'}",
            icon="CHECKMARK" if "Roughness" in aov_names else "X",
        )


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------


def register():
    bpy.types.Scene.dlsscomp_output_dir = bpy.props.StringProperty(
        name="Output Directory", subtype="DIR_PATH", default=""
    )
    bpy.utils.register_class(DLSSCOMP_OT_configure_passes)
    bpy.utils.register_class(DLSSCOMP_PT_export_panel)


def unregister():
    bpy.utils.unregister_class(DLSSCOMP_PT_export_panel)
    bpy.utils.unregister_class(DLSSCOMP_OT_configure_passes)
    del bpy.types.Scene.dlsscomp_output_dir


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
            print(
                "Passes configured: Combined, Z, Vector, Normal, "
                "DiffCol, GlossCol, Roughness(AOV)"
            )
            sys.exit(0)
    except Exception as e:
        import sys as _sys

        print(f"ERROR: {e}", file=_sys.stderr)
        sys.exit(1)
