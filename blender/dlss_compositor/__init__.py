bl_info = {
    "name": "DLSS Compositor",
    "blender": (5, 1, 0),
    "category": "Render",
    "version": (2, 0, 0),
}

from . import properties, panel, operators, aov_register
from .handlers import register_handlers, unregister_handlers

def register():
    properties.register()
    panel.register()
    operators.register()

def unregister():
    operators.unregister()
    panel.unregister()
    properties.unregister()
