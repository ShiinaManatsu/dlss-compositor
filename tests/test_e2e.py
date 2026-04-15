import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / "Release" / "dlss-compositor.exe"
INPUT_DIR = ROOT / "tests" / "fixtures" / "sequence"
OUTPUT_DIR = ROOT / "test_e2e_out"
VALIDATOR = ROOT / "tools" / "exr_validator.py"

pytestmark = pytest.mark.skipif(not EXE.exists(), reason="exe not built")


@pytest.fixture(scope="module", autouse=True)
def _cleanup_output_dir() -> None:
    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    yield
    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)


def _run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(EXE), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def _skip_if_no_rtx(result: subprocess.CompletedProcess[str]) -> None:
    combined = f"{result.stdout}\n{result.stderr}"
    if "DLSS-SR not available" in combined or "No compatible GPU" in combined:
        pytest.skip("No RTX GPU or DLSS-SR runtime available")


def _ensure_output_exists() -> None:
    if OUTPUT_DIR.exists() and any(OUTPUT_DIR.glob("*.exr")):
        return
    test_cli_processing()


def test_cli_processing() -> None:
    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    result = _run_cli(
        "--input-dir",
        str(INPUT_DIR),
        "--output-dir",
        str(OUTPUT_DIR),
        "--scale",
        "2",
    )

    _skip_if_no_rtx(result)
    assert result.returncode == 0, result.stderr or result.stdout

    count = sum(1 for _ in OUTPUT_DIR.glob("*.exr"))
    assert count == 5


def test_validator_on_output() -> None:
    _ensure_output_exists()

    result = subprocess.run(
        [sys.executable, str(VALIDATOR), str(OUTPUT_DIR)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr or result.stdout


def test_help_exits_zero() -> None:
    result = _run_cli("--help")
    assert result.returncode == 0, result.stderr or result.stdout
