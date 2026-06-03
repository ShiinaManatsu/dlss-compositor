from __future__ import annotations

bl_info = {
    "name": "DLSS Compositor",
    "blender": (5, 1, 0),
    "category": "Render",
    "version": (2, 0, 0),
}

# Blender requires bl_info before imports — E402 is expected here  # noqa: E402
from . import properties, panel, operators  # noqa: E402
from .handlers import register_handlers, unregister_handlers  # noqa: E402


def register() -> None:
    properties.register()
    panel.register()
    operators.register()
    register_handlers()


def unregister() -> None:
    unregister_handlers()
    operators.unregister()
    panel.unregister()
    properties.unregister()
