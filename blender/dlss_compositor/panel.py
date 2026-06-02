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
            layout.label(text="Enable DLSS Compositor to configure pipeline", icon='INFO')
            return

        # Quality Mode — horizontal toggle row (DLAA → UltraPerformance order per DLSS doc)
        layout.label(text="Quality Mode:")
        row = layout.row()
        row.prop(props, "quality_mode", expand=True)

        layout.separator()

        # Output Directory
        layout.prop(props, "output_dir")

        layout.separator()

        # Apply Settings button — configures passes + compositor + denoise + OCIO
        # NOTE: Does NOT modify resolution_percentage (that's handler's job at render time)
        layout.operator("dlss.apply_settings", icon='CHECKMARK')

        # Optional status widget showing current DLSS configuration
        from .quality_mapping import DLSS_RATIO, get_render_percentage
        pct = get_render_percentage(props.quality_mode)
        ratio = DLSS_RATIO[props.quality_mode]
        if ratio == 1.0:
            scale_str = "1× (no upscale)"
        else:
            scale_str = f"{ratio}× upscale"
        layout.label(text=f"Render at {pct}% → {scale_str}", icon='VIEW_CAMERA')


def register():
    bpy.utils.register_class(DLSS_PT_Main)

def unregister():
    bpy.utils.unregister_class(DLSS_PT_Main)
