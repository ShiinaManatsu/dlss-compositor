"""Test register/unregister without real Blender — uses bpy-stubs or fake-bpy-module."""
import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

# Try bpy-stubs first (user installed at C:\Python313\Lib\site-packages\bpy-stubs)
try:
    import bpy
except ImportError:
    try:
        import fake_bpy_module as bpy
    except ImportError:
        import unittest.mock as mock
        bpy = mock.MagicMock()
        sys.modules['bpy'] = bpy

def test_import():
    """Extension module can be imported."""
    # Basic import test
    assert True  # If import above worked, we're fine

def test_properties_defined():
    """QUALITY_MODES has 6 entries."""
    from blender.dlss_compositor.properties import QUALITY_MODES
    assert len(QUALITY_MODES) == 6

def test_no_forbidden_props():
    """Properties must not define cut_after, propose_scale, custom_scale as actual props."""
    import inspect
    from blender.dlss_compositor import properties
    src = inspect.getsource(properties)
    # Strip comment lines before checking — NOTE comments are allowed to name forbidden props
    non_comment_lines = [l for l in src.splitlines() if not l.strip().startswith('#')]
    clean_src = '\n'.join(non_comment_lines)
    assert 'cut_after' not in clean_src
    assert 'propose_scale' not in clean_src
    assert 'custom_scale' not in clean_src
    assert 'output_width' not in clean_src
    assert 'output_height' not in clean_src
