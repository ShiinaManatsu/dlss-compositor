"""Unit checks for frame-number Halton jitter. No Blender import."""
from __future__ import annotations

from jitter import JITTER_PHASES, halton, jitter_for_frame_number


def test_halton_known_values() -> None:
    assert abs(halton(1, 2) - 0.5) < 1e-9
    assert abs(halton(2, 2) - 0.25) < 1e-9
    assert abs(halton(3, 2) - 0.75) < 1e-9


def test_frame_1_is_phase_1() -> None:
    jx, jy = jitter_for_frame_number("BALANCED", 1)
    assert abs(jx - (halton(1, 2) - 0.5)) < 1e-9
    assert abs(jy - (halton(1, 3) - 0.5)) < 1e-9


def test_wraps_at_phase_count() -> None:
    n = JITTER_PHASES["BALANCED"]
    assert jitter_for_frame_number("BALANCED", 1) == jitter_for_frame_number("BALANCED", 1 + n)


if __name__ == "__main__":
    test_halton_known_values()
    test_frame_1_is_phase_1()
    test_wraps_at_phase_count()
    print("test_jitter ok")
