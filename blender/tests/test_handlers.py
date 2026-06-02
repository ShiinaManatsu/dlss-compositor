"""Tests for render handler lifecycle."""
import sys, os, json, tempfile
import unittest.mock
sys.modules['bpy'] = unittest.mock.MagicMock()

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
from unittest.mock import MagicMock, patch, call
from pathlib import Path


def make_mock_scene(enabled=True, quality_mode='PERFORMANCE', pct=80):
    scene = MagicMock()
    scene.frame_current = 1
    scene.render.resolution_percentage = pct
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080
    scene.render.fps = 30
    scene.display_settings.display_device = 'sRGB'
    scene.view_settings.view_transform = 'Filmic'
    scene.camera = None

    props = MagicMock()
    props.enabled = enabled
    props.quality_mode = quality_mode
    props.output_dir = tempfile.mkdtemp()

    scene.dlss_compositor = props

    # Simulate scene custom properties dict
    scene._custom_props = {}

    def setitem(_self, key, val):
        scene._custom_props[key] = val
    def getitem(_self, key):
        return scene._custom_props[key]
    def delitem(_self, key):
        del scene._custom_props[key]
    def contains(_self, key):
        return key in scene._custom_props

    scene.__setitem__ = setitem
    scene.__getitem__ = getitem
    scene.__delitem__ = delitem
    scene.__contains__ = contains

    return scene


class TestRenderInitLifecycle:
    def test_enabled_saves_and_overrides_pct(self):
        """render_init saves original pct and overrides with DLSS quality pct."""
        from blender.dlss_compositor.handlers import dlss_render_init

        scene = make_mock_scene(enabled=True, quality_mode='PERFORMANCE', pct=80)

        dlss_render_init(scene)

        # Saved original
        assert scene['_dlss_saved_pct'] == 80
        # Override to Performance (50%)
        assert scene.render.resolution_percentage == 50

    def test_disabled_no_op(self):
        """render_init when disabled must not touch anything."""
        from blender.dlss_compositor.handlers import dlss_render_init

        scene = make_mock_scene(enabled=False, pct=80)

        dlss_render_init(scene)

        # No custom prop set
        assert '_dlss_saved_pct' not in scene
        # Percentage unchanged
        assert scene.render.resolution_percentage == 80


class TestFinalizeLifecycle:
    def test_render_complete_restores_pct(self, tmp_path):
        """render_complete restores original percentage and deletes saved prop."""
        from blender.dlss_compositor.handlers import dlss_render_init, dlss_render_complete

        scene = make_mock_scene(enabled=True, quality_mode='PERFORMANCE', pct=80)
        scene.dlss_compositor.output_dir = str(tmp_path)

        # Initialize: override pct
        dlss_render_init(scene)
        assert scene.render.resolution_percentage == 50

        # Complete: restore pct
        dlss_render_complete(scene)
        assert scene.render.resolution_percentage == 80
        assert '_dlss_saved_pct' not in scene

    def test_render_cancel_restores_pct(self, tmp_path):
        """render_cancel also restores percentage."""
        from blender.dlss_compositor.handlers import dlss_render_init, dlss_render_cancel

        scene = make_mock_scene(enabled=True, quality_mode='PERFORMANCE', pct=75)
        scene.dlss_compositor.output_dir = str(tmp_path)

        dlss_render_init(scene)
        dlss_render_cancel(scene)

        assert scene.render.resolution_percentage == 75
        assert '_dlss_saved_pct' not in scene


class TestCrashRecovery:
    def test_load_post_restores_stale_pct(self):
        """load_post detects and restores stale _dlss_saved_pct after crash."""
        from blender.dlss_compositor.handlers import dlss_load_post

        scene = make_mock_scene(pct=50)  # After crash, pct stuck at DLSS override
        scene['_dlss_saved_pct'] = 80  # Stale saved value from before crash

        dlss_load_post(scene)

        # Restored to original
        assert scene.render.resolution_percentage == 80
        assert '_dlss_saved_pct' not in scene

    def test_load_post_no_op_if_no_stale(self):
        """load_post does nothing if no stale prop found."""
        from blender.dlss_compositor.handlers import dlss_load_post

        scene = make_mock_scene(pct=80)

        dlss_load_post(scene)

        assert scene.render.resolution_percentage == 80


class TestNoCutAfter:
    def test_no_cut_after_in_handlers(self):
        """Handlers must never write cut_after field."""
        import inspect
        from blender.dlss_compositor import handlers
        src = inspect.getsource(handlers)
        # Filter out comment lines
        non_comment_lines = [l for l in src.splitlines() if not l.strip().startswith('#')]
        non_comment = '\n'.join(non_comment_lines)
        assert "'cut_after'" not in non_comment
        assert '"cut_after"' not in non_comment


class TestPerFrameCamJson:
    def test_cam_json_written_for_each_frame(self, tmp_path):
        """render_pre writes per-frame cam.json files."""
        from blender.dlss_compositor.handlers import dlss_render_init, dlss_render_pre

        scene = make_mock_scene(enabled=True, quality_mode='PERFORMANCE', pct=80)
        scene.dlss_compositor.output_dir = str(tmp_path)

        dlss_render_init(scene)

        # Render 3 frames
        for frame in range(1, 4):
            scene.frame_current = frame
            dlss_render_pre(scene)

        # Check cam.json files written
        frames_dir = tmp_path / 'frames'
        cam_files = list(frames_dir.glob('*.cam.json'))
        assert len(cam_files) == 3
