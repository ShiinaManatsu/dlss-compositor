"""Unit checks for Stage 3 RAM-disk windows. No Blender / GPU."""
from __future__ import annotations

import json
from pathlib import Path

import run
from session_config import plan_ram_windows, ram_window_budget, resolve_render_percentage


class FakeRpc:
    def __init__(self, ram_dir: Path, delete_on_progress: bool = True) -> None:
        self.ram_dir = ram_dir
        self.delete_on_progress = delete_on_progress
        self.calls = 0

    def request(self, method: str, params: dict | None = None, timeout: float = 5.0) -> dict:
        self.calls += 1
        if method == "progress" and self.delete_on_progress:
            for path in self.ram_dir.rglob("*.exr"):
                if path.name.lower().startswith("preview_"):
                    continue
                path.unlink(missing_ok=True)
            return {"state": 1, "current": self.calls}
        if method == "progress":
            return {"state": 1}
        return {"ok": True}


def test_parse_args_default_is_quality_path() -> None:
    args = run.parse_args(["--blend", "shot.blend", "--export-dir", "build/out"])
    assert args.percentage is None
    assert args.legacy_scale is False
    assert args.native_res_buffers is False
    assert args.ram_fill_percent == 90
    assert args.ram_reserve == "1G"
    pct, reason = resolve_render_percentage(args.percentage, args.quality, legacy_scale=args.legacy_scale)
    assert pct is None
    assert reason == "blend"


def test_parse_args_native_res_buffers() -> None:
    args = run.parse_args(["--native-res-buffers", "--percentage", "58", "--mode", "sr"])
    assert args.native_res_buffers is True
    from session_config import wants_dual_pass

    assert wants_dual_pass(native_res_buffers=True, mode="sr", percentage=58) is True
    assert wants_dual_pass(native_res_buffers=True, mode="sr", percentage=100) is False


def test_build_blender_cmd_window_and_skip_config() -> None:
    args = run.parse_args(
        ["--blend", "shot.blend", "--percentage", "100", "--quality", "BALANCED", "--mode", "sr"]
    )
    blender = Path("blender.exe")
    cmd = run.build_blender_cmd(
        args,
        blender,
        Path("R:/dlss"),
        frame_start=4,
        frame_end=6,
        skip_config=True,
        config_frame_start=1,
        config_frame_end=10,
    )
    joined = " ".join(cmd)
    assert "--frame-start 4" in joined
    assert "--frame-end 6" in joined
    assert "--skip-config" in cmd
    assert "--config-frame-start" in cmd
    assert "--percentage" in cmd and "100" in cmd
    dual = run.build_blender_cmd(
        args,
        blender,
        Path("R:/dlss/color"),
        frame_start=4,
        frame_end=6,
        buffer_pass="beauty",
        percentage=58,
        config_dir=Path("R:/dlss"),
    )
    joined_dual = " ".join(dual)
    assert "--pass beauty" in joined_dual
    assert "--config-dir" in dual


def test_wait_exrs_consumed_deletes() -> None:
    ram = Path("build") / "test-ram-window"
    ram.mkdir(parents=True, exist_ok=True)
    for name in ("0001.exr", "0002.exr"):
        (ram / name).write_bytes(b"exr")
    (ram / "preview_0001.exr").write_bytes(b"skip")
    rpc = FakeRpc(ram)
    leftover, leftover_bytes = run.wait_exrs_consumed(ram, rpc, timeout=2.0)  # type: ignore[arg-type]
    assert leftover == []
    assert leftover_bytes == 0
    assert (ram / "preview_0001.exr").is_file()
    (ram / "preview_0001.exr").unlink()


def test_two_window_cycle_with_fake_exrs() -> None:
    ram = Path("build") / "test-ram-cycle"
    ram.mkdir(parents=True, exist_ok=True)
    for path in ram.glob("*.exr"):
        path.unlink()
    budget = ram_window_budget(64 * 1024**2, fill_percent=90, reserve_bytes=32 * 1024**2)
    per = 8 * 1024**2
    windows = plan_ram_windows(1, 6, per, budget, max_frames=3)
    assert windows == [(1, 3), (4, 6)]
    peaks: list[int] = []
    for start, end in windows:
        for frame in range(start, end + 1):
            (ram / f"{frame:04d}.exr").write_bytes(b"x" * 1024)
        files, used = run.scan_exrs(ram)
        peaks.append(used)
        assert used <= budget
        rpc = FakeRpc(ram)
        leftover, _ = run.wait_exrs_consumed(ram, rpc, timeout=2.0)  # type: ignore[arg-type]
        assert leftover == []
        assert run.scan_exrs(ram)[0] == []
    assert len(windows) >= 2
    assert max(peaks) <= budget


def test_format_bytes() -> None:
    assert run.format_bytes(1024) == "1.0 KB"
    assert "GB" in run.format_bytes(16 * 1024**3)


if __name__ == "__main__":
    test_parse_args_default_is_quality_path()
    test_parse_args_native_res_buffers()
    test_build_blender_cmd_window_and_skip_config()
    test_wait_exrs_consumed_deletes()
    test_two_window_cycle_with_fake_exrs()
    test_format_bytes()
    print("test_run_windows ok")
    print(json.dumps({"windows": 2, "ok": True}))
