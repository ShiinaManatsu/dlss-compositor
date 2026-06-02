"""
pytest configuration and fixtures for Blender extension tests.
Uses bpy-stubs (installed at C:\\Python313\\Lib\\site-packages\\bpy-stubs) or
fake-bpy-module as fallback.
"""
import sys
import os
import tempfile
from pathlib import Path
from unittest.mock import MagicMock

# Try to use real bpy-stubs if available
_bpy_stubs_path = r'C:\Python313\Lib\site-packages'
if os.path.exists(_bpy_stubs_path) and _bpy_stubs_path not in sys.path:
    sys.path.insert(0, _bpy_stubs_path)

# If bpy not importable (e.g., no stubs), create a comprehensive mock
if 'bpy' not in sys.modules:
    try:
        import bpy
    except ImportError:
        try:
            import fake_bpy_module as bpy
            sys.modules['bpy'] = bpy
        except ImportError:
            # Create minimal bpy mock
            bpy_mock = MagicMock()
            bpy_mock.app.handlers.render_init = []
            bpy_mock.app.handlers.render_pre = []
            bpy_mock.app.handlers.render_complete = []
            bpy_mock.app.handlers.render_cancel = []
            bpy_mock.app.handlers.load_post = []
            sys.modules['bpy'] = bpy_mock
            sys.modules['bpy.types'] = MagicMock()
            sys.modules['bpy.props'] = MagicMock()
            sys.modules['bpy.app'] = bpy_mock.app
            sys.modules['bpy.app.handlers'] = bpy_mock.app.handlers

import pytest

def _make_scene_dict_support(scene):
    """Add dict-like access to a mock scene for custom properties."""
    scene._custom_props = {}
    scene.__setitem__ = lambda s, k, v: scene._custom_props.__setitem__(k, v)
    scene.__getitem__ = lambda s, k: scene._custom_props.__getitem__(k)
    scene.__delitem__ = lambda s, k: scene._custom_props.__delitem__(k)
    scene.__contains__ = lambda s, k: k in scene._custom_props
    return scene

@pytest.fixture
def tmp_output_dir(tmp_path):
    """Temporary output directory for test renders."""
    return str(tmp_path)

@pytest.fixture
def fake_scene(tmp_output_dir):
    """Mock bpy.types.Scene with realistic render properties."""
    scene = MagicMock()
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080
    scene.render.resolution_percentage = 100
    scene.render.fps = 24
    scene.render.pixel_aspect_x = 1.0
    scene.render.pixel_aspect_y = 1.0
    scene.frame_current = 1
    scene.camera = None
    scene.use_nodes = False
    scene.node_tree = MagicMock()
    scene.node_tree.nodes = []
    scene.node_tree.links = MagicMock()
    scene.display_settings.display_device = 'sRGB'
    scene.view_settings.view_transform = 'Filmic'
    _make_scene_dict_support(scene)
    return scene

@pytest.fixture
def fake_render_props():
    """Mock DLSSCompositorProperties."""
    props = MagicMock()
    props.enabled = True
    props.quality_mode = 'PERFORMANCE'
    props.output_dir = tempfile.mkdtemp()
    return props

@pytest.fixture
def fake_scene_with_props(fake_scene, fake_render_props, tmp_output_dir):
    """Scene with DLSS properties attached."""
    fake_render_props.output_dir = tmp_output_dir
    fake_scene.dlss_compositor = fake_render_props
    return fake_scene

@pytest.fixture
def mock_camera_at():
    """Factory for mock cameras at given location/rotation."""
    def _make(loc=(0, 0, 5), rot=(0, 0, 0)):
        camera = MagicMock()
        camera.data.angle = 0.785398  # 45 degrees
        camera.data.clip_start = 0.1
        camera.data.clip_end = 100.0
        # Identity matrix_world
        camera.matrix_world = [[1,0,0,loc[0]],[0,1,0,loc[1]],[0,0,1,loc[2]],[0,0,0,1]]
        return camera
    return _make

@pytest.fixture
def mock_color_management():
    """Factory for scenes with specific color management settings."""
    def _make(display_device='sRGB', view_transform='Filmic'):
        scene = MagicMock()
        scene.display_settings.display_device = display_device
        scene.view_settings.view_transform = view_transform
        return scene
    return _make
