"""Tests for Halton(2,3) jitter implementation."""
import sys, os
import unittest.mock
sys.modules['bpy'] = unittest.mock.MagicMock()

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
import math
from blender.dlss_compositor.jitter import halton, generate_jitter_sequence, _clamp_jitter, CLAMP_MAX, CLAMP_MIN

class TestHaltonClassicValues:
    """Verify against known Halton sequence values."""
    
    def test_halton_base2_index1(self):
        assert halton(1, 2) == 0.5
    
    def test_halton_base2_index2(self):
        assert halton(2, 2) == 0.25
    
    def test_halton_base2_index3(self):
        assert halton(3, 2) == 0.75
    
    def test_halton_base3_index1(self):
        assert abs(halton(1, 3) - 1/3) < 1e-10
    
    def test_halton_base3_index2(self):
        assert abs(halton(2, 3) - 2/3) < 1e-10
    
    def test_halton_base3_index3(self):
        assert abs(halton(3, 3) - 1/9) < 1e-10

    def test_halton_positive(self):
        """All Halton values must be positive."""
        for i in range(1, 100):
            assert halton(i, 2) > 0
            assert halton(i, 3) > 0
    
    def test_halton_less_than_1(self):
        """All Halton values must be less than 1."""
        for i in range(1, 100):
            assert halton(i, 2) < 1.0
            assert halton(i, 3) < 1.0


class TestJitterRange:
    """Verify all generated jitter values stay within [-0.499, +0.499]."""
    
    def test_all_modes_in_range(self):
        for mode in ['DLAA', 'ULTRA_QUALITY', 'QUALITY', 'BALANCED', 'PERFORMANCE', 'ULTRA_PERFORMANCE']:
            seq = generate_jitter_sequence(mode, 100)
            for x, y in seq:
                assert CLAMP_MIN <= x <= CLAMP_MAX, f"{mode} jitter_x={x} out of range"
                assert CLAMP_MIN <= y <= CLAMP_MAX, f"{mode} jitter_y={y} out of range"


class TestJitterWrap:
    """Verify phase wrapping (sequence repeats at phase_count)."""
    
    def test_performance_wraps_at_32(self):
        seq = generate_jitter_sequence('PERFORMANCE', 65)  # > 2 × 32
        # Frame 0 and Frame 32 should be identical
        assert seq[0] == seq[32], f"Frame 0={seq[0]} != Frame 32={seq[32]}"
        # Frame 1 and Frame 33 should be identical
        assert seq[1] == seq[33]
    
    def test_dlaa_wraps_at_8(self):
        seq = generate_jitter_sequence('DLAA', 20)
        assert seq[0] == seq[8]
        assert seq[7] == seq[15]
    
    def test_ultra_performance_wraps_at_72(self):
        seq = generate_jitter_sequence('ULTRA_PERFORMANCE', 150)
        assert seq[0] == seq[72]


class TestJitterDistribution:
    """Verify jitter values are well-distributed across quadrants."""
    
    def test_performance_4_quadrant_coverage(self):
        """32-frame Performance sequence should cover all 4 quadrants."""
        seq = generate_jitter_sequence('PERFORMANCE', 32)
        q1 = sum(1 for x, y in seq if x > 0 and y > 0)
        q2 = sum(1 for x, y in seq if x < 0 and y > 0)
        q3 = sum(1 for x, y in seq if x < 0 and y < 0)
        q4 = sum(1 for x, y in seq if x > 0 and y < 0)
        # Each quadrant should have at least 6 samples (25% of 32 = 8, allow ±25%)
        assert q1 >= 6, f"Q1 has only {q1} samples"
        assert q2 >= 6, f"Q2 has only {q2} samples"
        assert q3 >= 6, f"Q3 has only {q3} samples"
        assert q4 >= 6, f"Q4 has only {q4} samples"


class TestPhaseCountTable:
    """Verify JITTER_PHASES matches DLSS SR §3.7.1.1 exactly."""
    from blender.dlss_compositor.quality_mapping import JITTER_PHASES
    
    def test_dlaa_8(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['DLAA'] == 8
    
    def test_ultra_quality_8(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['ULTRA_QUALITY'] == 8
    
    def test_quality_18(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['QUALITY'] == 18
    
    def test_balanced_24(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['BALANCED'] == 24
    
    def test_performance_32(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['PERFORMANCE'] == 32
    
    def test_ultra_performance_72(self):
        from blender.dlss_compositor.quality_mapping import JITTER_PHASES
        assert JITTER_PHASES['ULTRA_PERFORMANCE'] == 72


class TestClamp:
    """Verify jitter clamping edge cases."""
    
    def test_clamp_max(self):
        assert _clamp_jitter(0.5) == 0.499
    
    def test_clamp_max_extreme(self):
        assert _clamp_jitter(1.0) == 0.499
    
    def test_clamp_min(self):
        assert _clamp_jitter(-0.5) == -0.499
    
    def test_clamp_min_extreme(self):
        assert _clamp_jitter(-1.0) == -0.499
    
    def test_no_clamp_inside(self):
        assert _clamp_jitter(0.3) == 0.3
        assert _clamp_jitter(-0.3) == -0.3

    def test_no_base_1_usage(self):
        """Verify no base=1 is used anywhere in jitter.py."""
        import inspect
        from blender.dlss_compositor import jitter
        src = inspect.getsource(jitter)
        # Should not find halton calls with base=1
        # halton(x, 1) would always return 0 (degenerate)
        assert 'halton(' not in src or '1)' not in src.split('halton(')[1].split('\n')[0] or True
        # More specifically: no "halton(x, 1)" pattern in the sequence generation
        import re
        bad_patterns = re.findall(r'halton\(\w+,\s*1\)', src)
        assert len(bad_patterns) == 0, f"Found halton base=1 calls: {bad_patterns}"
