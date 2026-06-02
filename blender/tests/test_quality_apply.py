"""Tests for quality panel — Apply Settings operator + quality table cross-check."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
from unittest.mock import MagicMock, patch
import sys
sys.modules['bpy'] = MagicMock()

# Import quality_mapping directly (no bpy dependency)
from blender.dlss_compositor.quality_mapping import (
    DLSS_RATIO, JITTER_PHASES, get_render_percentage, get_render_resolution
)

class TestDlssRatioTable:
    def test_dlaa_ratio(self):
        assert DLSS_RATIO['DLAA'] == 1.0

    def test_quality_ratio(self):
        assert DLSS_RATIO['QUALITY'] == 1.5

    def test_balanced_ratio(self):
        assert abs(DLSS_RATIO['BALANCED'] - 1.724) < 1e-5

    def test_performance_ratio(self):
        assert DLSS_RATIO['PERFORMANCE'] == 2.0

    def test_ultra_performance_ratio(self):
        assert DLSS_RATIO['ULTRA_PERFORMANCE'] == 3.0

    def test_six_quality_modes(self):
        assert len(DLSS_RATIO) == 6

class TestJitterPhaseTable:
    def test_dlaa_phases(self):
        assert JITTER_PHASES['DLAA'] == 8

    def test_ultra_quality_phases(self):
        assert JITTER_PHASES['ULTRA_QUALITY'] == 8

    def test_quality_phases(self):
        assert JITTER_PHASES['QUALITY'] == 18

    def test_balanced_phases(self):
        assert JITTER_PHASES['BALANCED'] == 24

    def test_performance_phases(self):
        assert JITTER_PHASES['PERFORMANCE'] == 32

    def test_ultra_performance_phases(self):
        assert JITTER_PHASES['ULTRA_PERFORMANCE'] == 72

class TestRenderPercentage:
    def test_dlaa_100pct(self):
        assert get_render_percentage('DLAA') == 100

    def test_quality_67pct(self):
        assert get_render_percentage('QUALITY') == 67

    def test_performance_50pct(self):
        assert get_render_percentage('PERFORMANCE') == 50

    def test_ultra_performance_33pct(self):
        assert get_render_percentage('ULTRA_PERFORMANCE') == 33

class TestRenderResolution:
    def test_4k_balanced_integer_math(self):
        """4K Balanced: output=3840x2160, pct=58, render=2227x1253"""
        w, h = get_render_resolution(3840, 2160, 'BALANCED')
        assert w == 3840 * 58 // 100  # 2227
        assert h == 2160 * 58 // 100  # 1252 (integer floor)

class TestCrossCheckWithCppTypes:
    """Cross-check Python quality_mapping.py matches src/core/types.h constants."""
    def test_cross_check_cpp_constants(self):
        import re
        types_h = os.path.join(
            os.path.dirname(__file__), '..', '..', 'src', 'core', 'types.h'
        )
        if not os.path.exists(types_h):
            pytest.skip("src/core/types.h not found")

        content = open(types_h).read()
        # Extract DLSS_RATIO array values
        ratio_match = re.search(r'DLSS_RATIO\[\]\s*=\s*\{([^}]+)\}', content)
        if not ratio_match:
            pytest.skip("DLSS_RATIO array not found in types.h")

        cpp_ratios_str = ratio_match.group(1)
        cpp_ratios_str = re.sub(r'//.*', '', cpp_ratios_str)
        cpp_ratios = [float(x.strip().rstrip('f')) for x in cpp_ratios_str.split(',') if x.strip()]

        py_ratios = list(DLSS_RATIO.values())
        for i, (py_r, cpp_r) in enumerate(zip(py_ratios, cpp_ratios)):
            assert abs(py_r - cpp_r) < 1e-5, f"Mismatch at index {i}: Python={py_r}, C++={cpp_r}"
