import bpy

class DLSS_PT_Main(bpy.types.Panel):
    bl_label = "DLSS Compositor"
    bl_idname = "DLSS_PT_main"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        props = context.scene.dlss_compositor
        self.layout.prop(props, "enabled", text="")

    def draw(self, context):
        layout = self.layout
        props = context.scene.dlss_compositor
        if not props.enabled:
            layout.label(text="Enable DLSS Compositor in header")
            return
        layout.label(text="Quality, output dir and Apply Settings (T11)")

def register():
    bpy.utils.register_class(DLSS_PT_Main)

def unregister():
    bpy.utils.unregister_class(DLSS_PT_Main)
