from __future__ import annotations

import bpy
from bpy.types import (
    Context,
    NodeTree,
    CompositorNodeOutputFile,
    CompositorNodeRLayers,
    NodeCompositorFileOutputItem,
    Scene,
    Operator,
)


def _get_or_create_compositor_tree(scene: Scene) -> NodeTree | None:
    """Return (or create) the compositor NodeTree for *scene*.

    Blender 5.1: ``scene.node_tree`` removed; use ``scene.compositing_node_group``.
    ``scene.use_nodes`` deprecated — setting it has no effect (always True).
    Enable compositing via ``scene.render.use_compositing`` instead.
    """
    scene.render.use_compositing = True
    tree: NodeTree | None = scene.compositing_node_group
    if tree is None:
        tree = bpy.data.node_groups.new(name="Compositor", type="CompositorNodeTree")
        scene.compositing_node_group = tree
    return tree


class DLSS_OT_ApplySettings(Operator):
    """Configure render passes, compositor nodes, denoise, and OCIO for DLSS."""

    bl_idname = "dlss.apply_settings"
    bl_label = "Apply Settings"
    bl_description = (
        "Configure passes, compositor File Output nodes, "
        "Cycles denoise, and OCIO Raw for DLSS pipeline"
    )

    def execute(self, context: Context) -> set[str]:
        return bpy.ops.dlss.configure_all_passes()


class DLSS_OT_ConfigureAllPasses(Operator):
    """Configure all render passes and compositor nodes for the DLSS pipeline.

    This is the implementation backend called by the Apply Settings button.
    Also available for direct API/scripted use.
    """

    bl_idname = "dlss.configure_all_passes"
    bl_label = "Configure All Passes"
    bl_description = (
        "Enable Combined/Depth/Vector passes, configure compositor "
        "File Output nodes with Raw OCIO"
    )

    def execute(self, context: Context) -> set[str]:
        scene: Scene = context.scene
        props = scene.dlss_compositor  # type: ignore[attr-defined]

        # 1. Enable required render passes
        view_layer = context.view_layer
        view_layer.use_pass_z = True
        view_layer.use_pass_vector = True

        # 2. Force Cycles denoising
        if hasattr(scene, "cycles"):
            scene.cycles.use_denoising = True  # type: ignore[attr-defined]

        # 3. Set up compositor File Output nodes
        if self._setup_file_output_nodes(scene, props) is False:
            return {"CANCELLED"}

        self.report({"INFO"}, "DLSS passes and compositor configured")
        return {"FINISHED"}

    def _setup_file_output_nodes(self, scene: Scene, props) -> bool | None:  # type: ignore[no-untyped-def]
        """Create File Output nodes for Combined, Depth, Vector with Raw OCIO."""
        tree: NodeTree | None = _get_or_create_compositor_tree(scene)
        if tree is None:
            self.report(
                {"ERROR"},
                "Compositor node tree unavailable. "
                "Open a Compositor workspace and enable compositing, "
                "then re-run Apply Settings.",
            )
            return False

        nodes = tree.nodes
        links = tree.links
        output_dir: str = props.output_dir if props.output_dir else "/tmp/dlss/"

        # Find or create RenderLayers node
        rl_node: CompositorNodeRLayers | None = next(
            (n for n in nodes if n.type == "R_LAYERS"), None  # type: ignore[assignment]
        )
        if rl_node is None:
            rl_node = nodes.new("CompositorNodeRLayers")  # type: ignore[assignment]
            rl_node.location = (-300, 0)

        # Remove any existing DLSS output nodes (avoid duplicates on re-run)
        for node in [n for n in nodes if n.label and n.label.startswith("DLSS_")]:
            nodes.remove(node)

        # ONE multilayer EXR per frame — all passes in one file.
        # Channel names must match what frame_prefetcher.cpp expects:
        #   Combined → R, G, B, A  (item name "")
        #   Depth    → Z           (item name "Z")
        #   Vector   → Vector.X/Y/Z/W  (item name "Vector")
        fo: CompositorNodeOutputFile = nodes.new("CompositorNodeOutputFile")  # type: ignore[assignment]
        fo.label = "DLSS_Output"
        fo.location = (200, 0)
        fo.directory = output_dir
        fo.file_name = "####"

        fmt = fo.format
        if fmt is not None:
            fmt.file_format = "OPEN_EXR_MULTILAYER"
            fmt.color_management = "OVERRIDE"
            fmt.view_settings.view_transform = "Raw"

        # Clear default items and add the three passes
        fo.file_output_items.clear()
        item_combined: NodeCompositorFileOutputItem = fo.file_output_items.new("RGBA", "")
        item_combined.name = ""
        item_z: NodeCompositorFileOutputItem = fo.file_output_items.new("FLOAT", "Z")
        item_z.name = "Z"
        item_vec: NodeCompositorFileOutputItem = fo.file_output_items.new("VECTOR", "Vector")
        item_vec.name = "Vector"

        # Wire: Image→input[0], Depth→input[1], Vector→input[2]
        for out_name, inp_idx in [("Image", 0), ("Depth", 1), ("Vector", 2)]:
            sock = rl_node.outputs.get(out_name)  # type: ignore[union-attr]
            if sock and len(fo.inputs) > inp_idx:
                links.new(sock, fo.inputs[inp_idx])

        return None


def register() -> None:
    bpy.utils.register_class(DLSS_OT_ApplySettings)
    bpy.utils.register_class(DLSS_OT_ConfigureAllPasses)


def unregister() -> None:
    bpy.utils.unregister_class(DLSS_OT_ConfigureAllPasses)
    bpy.utils.unregister_class(DLSS_OT_ApplySettings)
