import os
import subprocess
import sys

VALIDATOR = os.path.join(os.path.dirname(__file__), "..", "tools", "exr_validator.py")
FIXTURE_GOOD = os.path.join(
    os.path.dirname(__file__), "fixtures", "reference_64x64.exr"
)
FIXTURE_MISSING = os.path.join(
    os.path.dirname(__file__), "fixtures", "missing_channels_64x64.exr"
)


def run_validator(*args):
    result = subprocess.run(
        [sys.executable, VALIDATOR, *args], capture_output=True, text=True
    )
    return result


def test_good_exr_passes():
    r = run_validator(FIXTURE_GOOD)
    assert r.returncode == 0
    assert "PASS" in r.stdout


def test_missing_channels_strict_fails():
    r = run_validator(FIXTURE_MISSING, "--strict")
    assert r.returncode == 1
    assert "FAIL" in r.stdout or "FAIL" in r.stderr


def test_missing_channels_nonstrict_fails():
    r = run_validator(FIXTURE_MISSING)
    assert r.returncode == 1


def test_nonexistent_file():
    r = run_validator("nonexistent_file.exr")
    assert r.returncode == 1


def test_directory_mode():
    fixture_dir = os.path.join(os.path.dirname(__file__), "fixtures", "sequence")
    r = run_validator(fixture_dir)
    assert r.returncode == 0
