"""Tests for Blender-side dlssconfig writer."""
import sys, os, json, tempfile
import unittest.mock
sys.modules['bpy'] = unittest.mock.MagicMock()

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
from unittest.mock import MagicMock, patch
from pathlib import Path

from blender.dlss_compositor.dlssconfig_writer import (
    build_top_level_config, merge_per_frame_jsons, validate_against_schema,
    write_atomic, write_dlssconfig, _clamp_jitter, HDR_DISPLAY_DEVICES
)


def make_mock_scene(display_device='sRGB', fps=30, res_x=3840, res_y=2160, pct=50):
    scene = MagicMock()
    scene.render.resolution_x = res_x
    scene.render.resolution_y = res_y
    scene.render.resolution_percentage = pct
    scene.render.fps = fps
    scene.display_settings.display_device = display_device
    scene.view_settings.view_transform = 'Filmic'
    return scene


def make_mock_props(quality_mode='PERFORMANCE'):
    props = MagicMock()
    props.quality_mode = quality_mode
    props.output_dir = '/tmp/test_dlss/'
    return props


class TestBuildTopLevelConfig:
    def test_basic_config_structure(self):
        scene = make_mock_scene()
        props = make_mock_props()
        config = build_top_level_config(scene, props)

        assert config['version'] == 1
        assert 'output_resolution' in config
        assert 'render_resolution' in config
        assert config['fps'] == 30
        assert config['is_hdr'] == False  # sRGB display

    def test_hdr_detection_pq(self):
        scene = make_mock_scene(display_device='Rec.2100-PQ')
        props = make_mock_props()
        config = build_top_level_config(scene, props)
        assert config['is_hdr'] == True

    def test_hdr_detection_hlg(self):
        scene = make_mock_scene(display_device='Rec.2100-HLG')
        props = make_mock_props()
        config = build_top_level_config(scene, props)
        assert config['is_hdr'] == True

    def test_srgb_not_hdr(self):
        scene = make_mock_scene(display_device='sRGB')
        props = make_mock_props()
        config = build_top_level_config(scene, props)
        assert config['is_hdr'] == False

    def test_render_resolution_integer_math(self):
        """4K Performance: output=3840x2160, pct=50 → render=1920x1080"""
        scene = make_mock_scene(res_x=3840, res_y=2160, pct=50)
        props = make_mock_props('PERFORMANCE')
        config = build_top_level_config(scene, props)
        assert config['render_resolution']['width'] == 1920
        assert config['render_resolution']['height'] == 1080

    def test_quality_mode_lowercase(self):
        scene = make_mock_scene()
        props = make_mock_props('ULTRA_PERFORMANCE')
        config = build_top_level_config(scene, props)
        assert config['quality_mode'] == 'ultra_performance'

    def test_jitter_phases_correct(self):
        scene = make_mock_scene()
        props = make_mock_props('PERFORMANCE')
        config = build_top_level_config(scene, props)
        assert config['jitter_phases'] == 32


class TestJitterClamp:
    def test_clamp_above_max(self):
        assert _clamp_jitter(0.7) == 0.499

    def test_clamp_below_min(self):
        assert _clamp_jitter(-0.7) == -0.499

    def test_no_clamp_inside(self):
        assert _clamp_jitter(0.3) == 0.3
        assert _clamp_jitter(-0.3) == -0.3

    def test_extreme_value_never_rejected(self):
        """Even extreme jitter values must be clamped, never rejected."""
        result = _clamp_jitter(1.0)
        assert result == 0.499  # Clamped, not raised

    def test_edge_at_boundary(self):
        """Values right at the boundary are clamped."""
        assert _clamp_jitter(0.5) == 0.499
        assert _clamp_jitter(-0.5) == -0.499


class TestMergePerFrameJsons:
    def test_merge_5_frames(self, tmp_path):
        frames_dir = tmp_path / 'frames'
        frames_dir.mkdir()

        for i in range(1, 6):
            frame_data = {
                'matrix_world': [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
                'projection': [[1,0,0,0],[0,1,0,0],[0,0,-1,0],[0,0,0,1]],
                'fov': 45.0, 'aspect_ratio': 1.777, 'near_clip': 0.1, 'far_clip': 100.0,
                'jitter_x': 0.25, 'jitter_y': -0.3
            }
            (frames_dir / f'{i:04d}.cam.json').write_text(json.dumps(frame_data))

        frames = merge_per_frame_jsons(str(tmp_path))
        assert len(frames) == 5
        assert '0001' in frames
        assert '0005' in frames

    def test_jitter_clamped_in_merge(self, tmp_path):
        frames_dir = tmp_path / 'frames'
        frames_dir.mkdir()
        frame_data = {'jitter_x': 0.7, 'jitter_y': -0.8,
                      'matrix_world': [[1,0,0,0]]*4, 'projection': [[1,0,0,0]]*4,
                      'fov': 45.0, 'aspect_ratio': 1.777, 'near_clip': 0.1, 'far_clip': 100.0}
        (frames_dir / '0001.cam.json').write_text(json.dumps(frame_data))

        frames = merge_per_frame_jsons(str(tmp_path))
        assert frames['0001']['jitter_x'] == 0.499
        assert frames['0001']['jitter_y'] == -0.499

    def test_empty_dir_returns_empty(self, tmp_path):
        """Frames dir doesn't exist → returns empty dict."""
        frames = merge_per_frame_jsons(str(tmp_path))
        assert frames == {}

    def test_invalid_json_skipped(self, tmp_path):
        """Corrupt .cam.json files are skipped with a warning."""
        frames_dir = tmp_path / 'frames'
        frames_dir.mkdir()
        (frames_dir / '0001.cam.json').write_text('{ not valid json')
        frames = merge_per_frame_jsons(str(tmp_path))
        assert frames == {}


class TestWriteAtomic:
    def test_write_creates_file(self, tmp_path):
        data = {'version': 1, 'test': True}
        out = str(tmp_path / 'test.json')
        write_atomic(out, data)
        assert os.path.exists(out)
        loaded = json.loads(open(out).read())
        assert loaded['version'] == 1

    def test_no_tmp_left_on_success(self, tmp_path):
        data = {'test': 1}
        out = str(tmp_path / 'test.json')
        write_atomic(out, data)
        assert not os.path.exists(out + '.tmp')

    def test_schema_not_duplicated_in_blender(self):
        """Schema must NOT be duplicated inside blender/ — only src/core/ has it."""
        import glob
        blender_schemas = glob.glob('blender/**/*.schema.json', recursive=True)
        assert len(blender_schemas) == 0, f"Schema duplicated in blender/: {blender_schemas}"


class TestValidateAgainstSchema:
    def _valid_config(self):
        return {
            'version': 1,
            'output_resolution': {'width': 3840, 'height': 2160},
            'render_resolution': {'width': 1920, 'height': 1080},
            'quality_mode': 'performance',
            'display_device': 'sRGB',
            'view_transform': 'Filmic',
            'is_hdr': False,
            'fps': 30,
            'filename_pattern': r'^\d{4}\.exr$',
            'jitter_phases': 32,
        }

    def test_valid_config_passes(self):
        """A fully valid config must not raise."""
        validate_against_schema(self._valid_config())  # no exception

    def test_reject_wrong_version(self):
        """version must be const 1."""
        cfg = self._valid_config()
        cfg['version'] = 2
        with pytest.raises(ValueError):
            validate_against_schema(cfg)

    def test_reject_unknown_quality_mode(self):
        """quality_mode must be from the enum."""
        cfg = self._valid_config()
        cfg['quality_mode'] = 'insane'
        with pytest.raises(ValueError):
            validate_against_schema(cfg)

    def test_reject_negative_fps(self):
        """fps minimum is 1."""
        cfg = self._valid_config()
        cfg['fps'] = 0
        with pytest.raises(ValueError):
            validate_against_schema(cfg)

    def test_reject_missing_required_field(self):
        """Missing required field raises."""
        cfg = self._valid_config()
        del cfg['is_hdr']
        with pytest.raises(ValueError):
            validate_against_schema(cfg)

    def test_reject_additional_properties(self):
        """additionalProperties: false — extra fields must fail."""
        cfg = self._valid_config()
        cfg['unknown_field'] = 'oops'
        with pytest.raises(ValueError):
            validate_against_schema(cfg)

    def test_reject_zero_resolution(self):
        """Resolution width/height must be >= 1 (positiveInteger)."""
        cfg = self._valid_config()
        cfg['output_resolution']['width'] = 0
        with pytest.raises(ValueError):
            validate_against_schema(cfg)


class TestWriteDlssconfig:
    def _valid_top_level(self):
        return {
            'version': 1,
            'output_resolution': {'width': 3840, 'height': 2160},
            'render_resolution': {'width': 1920, 'height': 1080},
            'quality_mode': 'performance',
            'display_device': 'sRGB',
            'view_transform': 'Filmic',
            'is_hdr': False,
            'fps': 30,
            'filename_pattern': r'^\d{4}\.exr$',
            'jitter_phases': 32,
        }

    def test_writes_valid_config(self, tmp_path):
        top = self._valid_top_level()
        frames = {}
        write_dlssconfig(str(tmp_path), top, frames)
        out = tmp_path / 'dlssconfig.json'
        assert out.exists()
        data = json.loads(out.read_text())
        assert data['version'] == 1
        assert 'frames' in data

    def test_invalid_config_not_written(self, tmp_path):
        """Invalid config must raise ValueError and NOT write the file."""
        top = self._valid_top_level()
        top['version'] = 99  # invalid
        with pytest.raises(ValueError):
            write_dlssconfig(str(tmp_path), top, {})
        assert not (tmp_path / 'dlssconfig.json').exists()

    def test_frames_included_in_output(self, tmp_path):
        top = self._valid_top_level()
        frames = {
            '0001': {
                'matrix_world': [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
                'projection': [[1,0,0,0],[0,1,0,0],[0,0,-1,0],[0,0,0,1]],
                'fov': 45.0, 'aspect_ratio': 1.777, 'near_clip': 0.1,
                'far_clip': 100.0, 'jitter_x': 0.1, 'jitter_y': -0.1
            }
        }
        write_dlssconfig(str(tmp_path), top, frames)
        data = json.loads((tmp_path / 'dlssconfig.json').read_text())
        assert '0001' in data['frames']
