import bpy

class DLSS_OT_ApplySettings(bpy.types.Operator):
    """Configure render passes, compositor nodes, denoise, and OCIO for DLSS."""
    bl_idname = "dlss.apply_settings"
    bl_label = "Apply Settings"
    bl_description = "Configure passes, compositor File Output nodes, Cycles denoise, and OCIO Raw for DLSS pipeline"

    def execute(self, context):
        return bpy.ops.dlss.configure_all_passes()

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


class DLSS_OT_ConfigureAllPasses(bpy.types.Operator):
    """Configure all render passes and compositor nodes for DLSS pipeline.

    This is the implementation backend called by Apply Settings button.
    Also available for direct API/scripted use.
    """
    bl_idname = "dlss.configure_all_passes"
    bl_label = "Configure All Passes"
    bl_description = "Enable Combined/Depth/Vector passes, configure compositor File Output nodes with Raw OCIO"

    def execute(self, context):
        scene = context.scene
        props = scene.dlss_compositor

        # 1. Enable required passes
        view_layer = context.view_layer
        view_layer.use_pass_z = True
        view_layer.use_pass_vector = True

        # 2. Force Cycles denoise
        if hasattr(scene, 'cycles'):
            scene.cycles.use_denoising = True

        # 3. Set up compositor File Output nodes
        self._setup_file_output_nodes(scene, props)

        self.report({'INFO'}, "DLSS passes and compositor configured")
        return {'FINISHED'}

    def _setup_file_output_nodes(self, scene, props):
        """Create File Output nodes for Combined, Depth, Vector with Raw OCIO."""
        scene.use_nodes = True
        tree = scene.node_tree
        nodes = tree.nodes
        links = tree.links

        output_dir = props.output_dir if props.output_dir else "/tmp/dlss/"

        # Find RenderLayers node
        rl_node = next((n for n in nodes if n.type == 'R_LAYERS'), None)
        if rl_node is None:
            rl_node = nodes.new('CompositorNodeRLayers')
            rl_node.location = (-300, 0)

        # Remove existing DLSS output nodes
        for node in [n for n in nodes if n.label and n.label.startswith('DLSS_')]:
            nodes.remove(node)

        def make_output_node(label, x, y, slot_name, output_socket):
            fo = nodes.new('CompositorNodeOutputFile')
            fo.label = label
            fo.location = (x, y)
            fo.base_path = output_dir
            fo.format.file_format = 'OPEN_EXR'
            fo.format.color_management = 'OVERRIDE'
            fo.format.view_settings.view_transform = 'Raw'
            if fo.file_slots:
                fo.file_slots[0].path = slot_name
            else:
                fo.file_slots.new(slot_name)
            sock = rl_node.outputs.get(output_socket)
            if sock:
                links.new(sock, fo.inputs[0])
            return fo

        make_output_node('DLSS_Combined', 200, 100, '####', 'Image')
        make_output_node('DLSS_Depth', 200, -100, '####.depth', 'Depth')
        make_output_node('DLSS_Vector', 200, -300, '####.vector', 'Vector')


def register():
    bpy.utils.register_class(DLSS_OT_ApplySettings)
    bpy.utils.register_class(DLSS_OT_ConfigureAllPasses)

def unregister():
    bpy.utils.unregister_class(DLSS_OT_ConfigureAllPasses)
    bpy.utils.unregister_class(DLSS_OT_ApplySettings)
