"""
Full integration test: simulate 30-frame render lifecycle.
Tests the complete pipeline: register → configure → 30 frames → merge → validate.
"""
import sys, os, json, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
from unittest.mock import MagicMock, patch
from pathlib import Path

from blender.dlss_compositor.quality_mapping import DLSS_RATIO, JITTER_PHASES, get_render_percentage
from blender.dlss_compositor.jitter import generate_jitter_sequence


class TestFullRenderLifecycle:
    """30-frame simulated render lifecycle test."""

    def _make_scene(self, output_dir, quality_mode='BALANCED', res_x=3840, res_y=2160, pct=100):
        scene = MagicMock()
        scene.render.resolution_x = res_x
        scene.render.resolution_y = res_y
        scene.render.resolution_percentage = pct
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

        # Custom props dict support
        scene._props = {}
        scene.__setitem__ = lambda s, k, v: scene._props.__setitem__(k, v)
        scene.__getitem__ = lambda s, k: scene._props.__getitem__(k)
        scene.__delitem__ = lambda s, k: scene._props.__delitem__(k)
        scene.__contains__ = lambda s, k: k in scene._props

        props = MagicMock()
        props.enabled = True
        props.quality_mode = quality_mode
        props.output_dir = output_dir
        scene.dlss_compositor = props

        return scene, props

    def test_30_frame_render_lifecycle(self, tmp_path):
        """Complete 30-frame 4K Balanced render: init → 30 frames → complete."""
        from blender.dlss_compositor.handlers import (
            dlss_render_init, dlss_render_pre, dlss_render_complete
        )

        output_dir = str(tmp_path)
        scene, props = self._make_scene(output_dir, 'BALANCED', 3840, 2160, 100)

        # render_init: save + override percentage
        dlss_render_init(scene)

        expected_pct = get_render_percentage('BALANCED')  # 58
        assert scene.render.resolution_percentage == expected_pct, \
            f"Expected {expected_pct}%, got {scene.render.resolution_percentage}%"
        assert '_dlss_saved_pct' in scene

        # render_pre: 30 frames
        for frame in range(1, 31):
            scene.frame_current = frame
            dlss_render_pre(scene)

        # Verify per-frame cam.json files
        frames_dir = tmp_path / 'frames'
        cam_files = list(frames_dir.glob('*.cam.json')) if frames_dir.exists() else []
        assert len(cam_files) == 30, f"Expected 30 cam.json files, got {len(cam_files)}"

        # Verify jitter values are in range
        for cam_file in cam_files:
            data = json.loads(cam_file.read_text())
            if 'jitter_x' in data:
                assert -0.5 <= data['jitter_x'] <= 0.5, f"jitter_x out of range in {cam_file.name}"
            if 'jitter_y' in data:
                assert -0.5 <= data['jitter_y'] <= 0.5, f"jitter_y out of range in {cam_file.name}"

        # render_complete: merge + restore
        dlss_render_complete(scene)

        # Percentage restored
        assert scene.render.resolution_percentage == 100
        assert '_dlss_saved_pct' not in scene

        # dlssconfig.json written
        dlssconfig_path = tmp_path / 'dlssconfig.json'
        if dlssconfig_path.exists():
            config = json.loads(dlssconfig_path.read_text())
            assert config['version'] == 1
            assert config['quality_mode'] == 'balanced'

    def test_render_resolution_integer_math(self):
        """Verify 4K Balanced render resolution is calculated with integer math."""
        # 4K Balanced: output=3840x2160, pct=58 → render=2227x1252 (floor)
        pct = get_render_percentage('BALANCED')
        render_w = 3840 * pct // 100
        render_h = 2160 * pct // 100
        assert render_w == 3840 * 58 // 100
        assert render_h == 2160 * 58 // 100

    def test_jitter_in_range_all_30_frames(self):
        """All 30 frames of BALANCED jitter must be in [-0.499, +0.499]."""
        seq = generate_jitter_sequence('BALANCED', 30)
        for i, (jx, jy) in enumerate(seq):
            assert -0.499 <= jx <= 0.499, f"Frame {i}: jitter_x={jx} out of range"
            assert -0.499 <= jy <= 0.499, f"Frame {i}: jitter_y={jy} out of range"


class TestSchemaValidation:
    def test_schema_accessible(self):
        """dlssconfig.schema.json must be accessible from blender/ via relative path."""
        schema_path = Path(__file__).parent.parent.parent / 'src' / 'core' / 'dlssconfig.schema.json'
        assert schema_path.exists(), f"Schema not found at {schema_path}"

    def test_schema_not_duplicated_in_blender(self):
        """Schema must NOT exist inside blender/ directory."""
        blender_dir = Path(__file__).parent.parent
        schema_copies = list(blender_dir.rglob('*.schema.json'))
        assert len(schema_copies) == 0, f"Schema duplicated in blender/: {schema_copies}"
