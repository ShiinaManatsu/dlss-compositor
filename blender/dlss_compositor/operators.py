import bpy
from . import aov_register
from .quality_mapping import DLSS_RATIO, JITTER_PHASES

class DLSS_OT_ApplySettings(bpy.types.Operator):
    """Configure render passes, compositor nodes, denoise, and OCIO for DLSS."""
    bl_idname = "dlss.apply_settings"
    bl_label = "Apply Settings"
    bl_description = "Configure passes, compositor File Output nodes, Cycles denoise, and OCIO Raw for DLSS pipeline"

    def execute(self, context):
        scene = context.scene
        props = scene.dlss_compositor

        # 1. Configure AOV passes (Z + Vector)
        aov_register.configure_aov_passes(context)

        # 2. Force Cycles denoise
        aov_register.configure_cycles_denoise(scene)

        # 3. Configure compositor File Output nodes
        self._setup_compositor(scene, props)

        # NOTE: Do NOT modify scene.render.resolution_percentage here!
        # That is handled transparently by the render_init handler (T14)
        # at render time: save → override → restore

        self.report({'INFO'}, f"DLSS Compositor configured for {props.quality_mode}")
        return {'FINISHED'}

    def _setup_compositor(self, scene, props):
        """Create/update File Output nodes in compositor for Combined/Depth/Vector."""
        scene.use_nodes = True
        tree = scene.node_tree
        nodes = tree.nodes
        links = tree.links

        # Find or create RenderLayers node
        render_layers = None
        for node in nodes:
            if node.type == 'R_LAYERS':
                render_layers = node
                break
        if render_layers is None:
            render_layers = nodes.new('CompositorNodeRLayers')
            render_layers.location = (-300, 0)

        output_dir = props.output_dir if props.output_dir else "/tmp/dlss_output/"

        # Remove old DLSS output nodes to avoid duplicates
        for node in list(nodes):
            if node.label and node.label.startswith("DLSS_"):
                nodes.remove(node)

        # Combined output
        fo_combined = nodes.new('CompositorNodeOutputFile')
        fo_combined.label = "DLSS_Combined"
        fo_combined.location = (200, 100)
        fo_combined.base_path = output_dir
        fo_combined.format.file_format = 'OPEN_EXR'
        fo_combined.format.color_depth = '16'
        fo_combined.format.color_management = 'OVERRIDE'
        fo_combined.format.view_settings.view_transform = 'Raw'
        if 'Image' not in fo_combined.file_slots:
            fo_combined.file_slots.new('####')
        if render_layers.outputs.get('Image'):
            links.new(render_layers.outputs['Image'], fo_combined.inputs[0])

        # Depth output
        fo_depth = nodes.new('CompositorNodeOutputFile')
        fo_depth.label = "DLSS_Depth"
        fo_depth.location = (200, -100)
        fo_depth.base_path = output_dir
        fo_depth.format.file_format = 'OPEN_EXR'
        fo_depth.format.color_management = 'OVERRIDE'
        fo_depth.format.view_settings.view_transform = 'Raw'
        if 'Depth' not in fo_depth.file_slots:
            fo_depth.file_slots.new('####.depth')
        if render_layers.outputs.get('Depth'):
            links.new(render_layers.outputs['Depth'], fo_depth.inputs[0])

        # Vector output
        fo_vector = nodes.new('CompositorNodeOutputFile')
        fo_vector.label = "DLSS_Vector"
        fo_vector.location = (200, -300)
        fo_vector.base_path = output_dir
        fo_vector.format.file_format = 'OPEN_EXR'
        fo_vector.format.color_management = 'OVERRIDE'
        fo_vector.format.view_settings.view_transform = 'Raw'
        if 'Vector' not in fo_vector.file_slots:
            fo_vector.file_slots.new('####.vector')
        if render_layers.outputs.get('Vector'):
            links.new(render_layers.outputs['Vector'], fo_vector.inputs[0])


def register():
    bpy.utils.register_class(DLSS_OT_ApplySettings)

def unregister():
    bpy.utils.unregister_class(DLSS_OT_ApplySettings)
