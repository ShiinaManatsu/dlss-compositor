"""
Cross-check: verify Python quality_mapping.py constants match src/core/types.h exactly.
Run as a test: python -m pytest blender/tests/cross_check_constants.py -v
"""
import re
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

import pytest
from blender.dlss_compositor.quality_mapping import DLSS_RATIO, JITTER_PHASES


def get_cpp_constants():
    """Parse src/core/types.h and extract DLSS_RATIO and JITTER_PHASES arrays."""
    types_h = os.path.join(
        os.path.dirname(__file__), '..', '..', 'src', 'core', 'types.h'
    )
    if not os.path.exists(types_h):
        return None, None

    content = open(types_h).read()

    # Extract DLSS_RATIO array
    ratio_match = re.search(r'DLSS_RATIO\[\]\s*=\s*\{([^}]+)\}', content)
    cpp_ratios = None
    if ratio_match:
        vals = [v.strip().rstrip('f').rstrip('F') for v in ratio_match.group(1).split(',') if v.strip()]
        cpp_ratios = [float(v) for v in vals if v]

    # Extract JITTER_PHASES array
    phases_match = re.search(r'JITTER_PHASES\[\]\s*=\s*\{([^}]+)\}', content)
    cpp_phases = None
    if phases_match:
        vals = [v.strip() for v in phases_match.group(1).split(',') if v.strip()]
        cpp_phases = [int(v) for v in vals if v.split('//')[0].strip().isdigit()
                      or (v.split('//')[0].strip().lstrip('-').isdigit())]

    return cpp_ratios, cpp_phases


class TestCrossCheckConstants:
    """Verify Python quality_mapping.py matches src/core/types.h."""

    def test_dlss_ratio_matches_cpp(self):
        cpp_ratios, _ = get_cpp_constants()
        if cpp_ratios is None:
            pytest.skip("src/core/types.h not found or DLSS_RATIO not parseable")

        py_ratios = list(DLSS_RATIO.values())

        assert len(py_ratios) == len(cpp_ratios), \
            f"Length mismatch: Python has {len(py_ratios)}, C++ has {len(cpp_ratios)}"

        for i, (py_r, cpp_r) in enumerate(zip(py_ratios, cpp_ratios)):
            quality = list(DLSS_RATIO.keys())[i]
            assert abs(py_r - cpp_r) < 1e-5, \
                f"Mismatch for {quality}: Python={py_r}, C++={cpp_r}"

    def test_jitter_phases_matches_cpp(self):
        _, cpp_phases = get_cpp_constants()
        if cpp_phases is None:
            pytest.skip("src/core/types.h not found or JITTER_PHASES not parseable")

        py_phases = list(JITTER_PHASES.values())

        assert len(py_phases) == len(cpp_phases), \
            f"Length mismatch: Python has {len(py_phases)}, C++ has {len(cpp_phases)}"

        for i, (py_p, cpp_p) in enumerate(zip(py_phases, cpp_phases)):
            quality = list(JITTER_PHASES.keys())[i]
            assert py_p == cpp_p, \
                f"Mismatch for {quality}: Python={py_p}, C++={cpp_p}"

    def test_python_table_sizes(self):
        """Both tables must have exactly 6 entries (one per quality mode)."""
        assert len(DLSS_RATIO) == 6
        assert len(JITTER_PHASES) == 6

    def test_known_values_sanity(self):
        """Spot-check known DLSS SR §3.7.1.1 values."""
        assert DLSS_RATIO['DLAA'] == 1.0
        assert DLSS_RATIO['PERFORMANCE'] == 2.0
        assert DLSS_RATIO['ULTRA_PERFORMANCE'] == 3.0
        assert JITTER_PHASES['ULTRA_PERFORMANCE'] == 72
        assert JITTER_PHASES['PERFORMANCE'] == 32
        assert JITTER_PHASES['BALANCED'] == 24
