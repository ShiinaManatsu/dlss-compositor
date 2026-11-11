"""pipeline-v3 orchestrator.

Stage 2: create ImDisk, inject, unmount.
Stage 3 (default): attach the member-mounted DLSSRAM volume, never create or
unmount, spawn long-lived dlss-compositor-cli JSON-RPC, consume EXRs as they
land, delete after use, write MOV/sequence to the working disk.

Release installer (not this script) should install ImDisk Toolkit. GUI/CLI
later needs an elevated token for `imdisk -p` NTFS format. This stage does
not request UAC and does not format.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from collections.abc import Callable
from typing import Any

from ramdisk import (
    DEFAULT_SIZE,
    IMDISK_MISSING_MESSAGE,
    VOLUME_LABEL,
    ImDiskError,
    RamDisk,
    create as create_ramdisk,
    discover,
    emit,
    parse_size,
    unmount,
    volume_space,
)
from rpc_client import RpcError, StdioRpcClient
from inspect_scene import inject_override_args
from session_config import (
    COLOR_SUBDIR,
    DEFAULT_FILENAME_TEMPLATE,
    DEFAULT_RAM_FILL_PERCENT,
    DEFAULT_RAM_RESERVE_BYTES,
    EXR_COLOR_DEPTH,
    GUIDES_SUBDIR,
    NR_RESERVED_MESSAGE,
    QUALITY_SLOTS,
    cycles_policy_for_mode,
    estimate_window_frame_bytes,
    job_token_from_blend,
    parse_fg_multiplier,
    phase_label,
    plan_ram_windows,
    ram_window_budget,
    resolve_filename_template,
    resolve_render_percentage,
    wants_dual_pass,
)

BLENDER_CANDIDATES = (
    Path(r"D:\Games\SteamLibrary\steamapps\common\Blender\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.3\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe"),
)

CLI_CANDIDATES = (
    Path("build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"),
    Path("build/review/Release/dlss-compositor-cli.exe"),
    Path("build/Release/dlss-compositor-cli.exe"),
)

INJECT_PATH = Path(__file__).resolve().parent / "inject.py"
INSPECT_PATH = Path(__file__).resolve().parent / "inspect_blend.py"


def find_blender(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError(f"blender not found: {path}")
        return path
    which = os.environ.get("BLENDER_EXE")
    if which and Path(which).is_file():
        return Path(which)
    import shutil

    found = shutil.which("blender")
    if found:
        return Path(found)
    for candidate in BLENDER_CANDIDATES:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("blender.exe not found; pass --blender")


def find_cli(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError(f"dlss-compositor-cli not found: {path}")
        return path
    env = os.environ.get("DLSS_COMPOSITOR_CLI")
    if env and Path(env).is_file():
        return Path(env)
    repo = Path(__file__).resolve().parents[1]
    for candidate in CLI_CANDIDATES:
        path = candidate if candidate.is_absolute() else repo / candidate
        if path.is_file():
            return path
    raise FileNotFoundError("dlss-compositor-cli.exe not found; pass --cli or set DLSS_COMPOSITOR_CLI")


def kill_tree(proc: subprocess.Popen[Any] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    emit("killing", pid=proc.pid)
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            check=False,
            capture_output=True,
        )
    else:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def inspect_blend(blender: Path, blend: Path, json_out: Path, timeout: float = 240.0) -> dict[str, Any]:
    """Fresh `blender -b` inspect. Never uses a member-owned GUI instance."""
    json_out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(blender),
        "-b",
        str(blend),
        "--python-exit-code",
        "1",
        "--python",
        str(INSPECT_PATH),
        "--",
        "--json-out",
        str(json_out),
    ]
    emit("inspect_start", cmd=cmd, blend=str(blend))
    completed = subprocess.run(
        cmd,
        cwd=str(Path(__file__).resolve().parents[1]),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    if json_out.is_file():
        payload = json.loads(json_out.read_text(encoding="utf-8"))
        emit("inspect", **payload)
        if payload.get("ok") is False:
            raise RuntimeError(str(payload.get("error") or "inspect failed"))
        return payload
    tail = (completed.stderr or completed.stdout or "")[-800:]
    raise RuntimeError(f"inspect 没有写出 JSON（exit {completed.returncode}）: {tail}")


def parse_blender_line(line: str) -> dict[str, object] | None:
    stripped = line.strip()
    if stripped.startswith("DLSS_EVENT:"):
        try:
            return json.loads(stripped[len("DLSS_EVENT:") :])
        except json.JSONDecodeError:
            return {"event": "blender_log", "line": stripped}
    import re

    match = re.search(r"Fra:(\d+)", stripped)
    if match:
        return {"event": "blender_frame", "frame": int(match.group(1)), "line": stripped[:200]}
    return None


def build_blender_cmd(
    args: argparse.Namespace,
    blender: Path,
    output_dir: Path,
    *,
    frame_start: int | None = None,
    frame_end: int | None = None,
    dry_run: bool | None = None,
    skip_config: bool = False,
    config_frame_start: int | None = None,
    config_frame_end: int | None = None,
    buffer_pass: str = "combined",
    percentage: int | None = None,
    config_dir: Path | None = None,
) -> list[str]:
    start = args.frame_start if frame_start is None else frame_start
    end = args.frame_end if frame_end is None else frame_end
    pct = args.percentage if percentage is None else percentage
    cmd = [str(blender), "-b"]
    if args.blend:
        cmd.append(str(Path(args.blend)))
    else:
        cmd.append("--factory-startup")
    cmd.extend(["--python", str(INJECT_PATH), "--", "--output-dir", str(output_dir)])
    cmd.extend(
        inject_override_args(
            {
                "resolution_percentage": pct,
                "frame_start": start,
                "frame_end": end,
                "samples": args.samples,
                "use_adaptive_sampling": getattr(args, "use_adaptive_sampling", None),
                "adaptive_threshold": getattr(args, "adaptive_threshold", None),
                "adaptive_min_samples": getattr(args, "adaptive_min_samples", None),
                "time_limit": getattr(args, "time_limit", None),
                "use_denoising": getattr(args, "use_denoising", None),
                "denoiser": getattr(args, "denoiser", None),
            }
        )
    )
    if args.quality:
        cmd.extend(["--quality", args.quality])
    use_dry = args.dry_run if dry_run is None else dry_run
    if use_dry:
        cmd.append("--dry-run")
    if skip_config:
        cmd.append("--skip-config")
    if config_frame_start is not None:
        cmd.extend(["--config-frame-start", str(config_frame_start)])
    if config_frame_end is not None:
        cmd.extend(["--config-frame-end", str(config_frame_end)])
    if args.engine:
        cmd.extend(["--engine", args.engine])
    cmd.extend(["--mode", args.mode])
    if getattr(args, "sampling_pattern", None):
        cmd.extend(["--sampling-pattern", str(args.sampling_pattern)])
    if getattr(args, "output_width", None) is not None:
        cmd.extend(["--output-width", str(args.output_width)])
    if getattr(args, "output_height", None) is not None:
        cmd.extend(["--output-height", str(args.output_height)])
    if getattr(args, "fps", None) is not None:
        cmd.extend(["--fps", str(args.fps)])
    if getattr(args, "view_transform", None):
        cmd.extend(["--view-transform", args.view_transform])
    if getattr(args, "display_device", None):
        cmd.extend(["--display-device", args.display_device])
    if getattr(args, "look", None):
        cmd.extend(["--look", args.look])
    if buffer_pass and buffer_pass != "combined":
        cmd.extend(["--pass", buffer_pass])
    if config_dir is not None:
        cmd.extend(["--config-dir", str(config_dir)])
    return cmd


def format_bytes(n: int) -> str:
    value = float(max(0, int(n)))
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024.0 or unit == "TB":
            if unit == "B":
                return f"{int(value)} {unit}"
            return f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{int(n)} B"


def wait_exrs_consumed(
    ram_dir: Path,
    rpc: StdioRpcClient,
    *,
    timeout: float,
    stop_flag: Callable[[], bool] | None = None,
) -> tuple[list[dict[str, object]], int]:
    """Wait until consumed EXRs are deleted. Returns leftover (files, bytes)."""
    deadline = time.time() + max(5.0, float(timeout))
    leftover, leftover_bytes = scan_exrs(ram_dir)
    while time.time() < deadline:
        leftover, leftover_bytes = scan_exrs(ram_dir)
        if not leftover:
            return leftover, leftover_bytes
        if stop_flag is not None and stop_flag():
            break
        try:
            progress = rpc.request("progress", {}, timeout=5.0)
            emit(
                "engine_progress",
                message="等待 DLSS 消费并删除 EXR",
                **progress,
                ramdisk_bytes=leftover_bytes,
                ramdisk_files=len(leftover),
            )
            state = progress.get("state")
            if state in (2, 3, "done", "error", 0, "idle"):
                leftover, leftover_bytes = scan_exrs(ram_dir)
                break
        except RpcError as exc:
            emit("rpc_warn", message=str(exc))
            break
        time.sleep(0.25)
    return leftover, leftover_bytes


def pump_blender(
    proc: subprocess.Popen[str],
    ram_dir: Path,
    *,
    budget_bytes: int,
    rpc: StdioRpcClient | None = None,
    stop_flag: Callable[[], bool] | None = None,
) -> tuple[int, int, int, int]:
    """Stream Blender logs; track RAM-disk peak. Returns exit, peak_bytes, peak_files, measured_exr."""
    assert proc.stdout is not None
    peak_bytes = 0
    peak_files = 0
    measured = 0
    while True:
        line = proc.stdout.readline()
        if line:
            parsed = parse_blender_line(line)
            if parsed is not None:
                emit(
                    "progress" if parsed.get("event") == "progress" else str(parsed.get("event")),
                    **{k: v for k, v in parsed.items() if k != "event"},
                )
            else:
                text = line.rstrip()
                if text:
                    emit("blender_log", line=text[:300])
        elif proc.poll() is not None:
            break

        files, used = scan_exrs(ram_dir)
        peak_bytes = max(peak_bytes, used)
        peak_files = max(peak_files, len(files))
        if files:
            measured = max(measured, max(int(item["size"]) for item in files))
        if used > budget_bytes:
            emit(
                "ram_window_over",
                message=f"内存盘已用 {format_bytes(used)}，超过预算 {format_bytes(budget_bytes)}",
                used=used,
                budget=budget_bytes,
            )
        if rpc is not None:
            try:
                progress = rpc.request("progress", {}, timeout=5.0)
                emit(
                    "engine_progress",
                    **progress,
                    ramdisk_bytes=used,
                    ramdisk_files=len(files),
                )
            except RpcError as exc:
                emit("rpc_warn", message=str(exc))
        if stop_flag is not None and stop_flag():
            kill_tree(proc)
            break
        if not line:
            time.sleep(0.05)
    return proc.wait(), peak_bytes, peak_files, measured


def scan_exrs(root: Path) -> tuple[list[dict[str, object]], int]:
    files: list[dict[str, object]] = []
    total = 0
    if not root.exists():
        return files, total
    for path in sorted(root.rglob("*.exr")):
        if path.name.lower().startswith("preview_"):
            continue
        size = path.stat().st_size
        files.append({"name": path.name, "size": size})
        total += size
    return files, total


def run_stage2(args: argparse.Namespace) -> int:
    blender = find_blender(args.blender)
    disk: RamDisk | None = None
    proc: subprocess.Popen[str] | None = None
    exit_code = 0

    def handle_stop(_signum: int, _frame: object) -> None:
        emit("signal", message="stop requested")
        kill_tree(proc)

    if hasattr(signal, "SIGINT"):
        signal.signal(signal.SIGINT, handle_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, handle_stop)

    try:
        disk = create_ramdisk(size=args.size, letter=args.letter)
        out_dir = disk.root / "dlss"
        out_dir.mkdir(parents=True, exist_ok=True)
        cmd = build_blender_cmd(args, blender, out_dir)
        emit("blender_start", cmd=cmd, pid_pending=True, output_dir=str(out_dir))
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        emit("blender_pid", pid=proc.pid)
        assert proc.stdout is not None
        for line in proc.stdout:
            parsed = parse_blender_line(line)
            if parsed is not None:
                emit(
                    "progress" if parsed.get("event") == "progress" else str(parsed.get("event")),
                    **{k: v for k, v in parsed.items() if k != "event"},
                )
            else:
                text = line.rstrip()
                if text:
                    emit("blender_log", line=text[:300])
        exit_code = proc.wait()
        emit("blender_exit", code=exit_code)
        return exit_code
    except ImDiskError as exc:
        emit("error", message=str(exc), missing=exc.missing)
        print(str(exc), file=sys.stderr)
        return 2 if exc.missing else 1
    except KeyboardInterrupt:
        emit("interrupted")
        kill_tree(proc)
        return 130
    finally:
        if proc is not None and proc.poll() is None:
            kill_tree(proc)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        if disk is not None:
            try:
                scan_root = disk.root / "dlss"
                contents, _ = scan_exrs(scan_root)
                emit("ramdisk_contents", letter=disk.letter, root=str(scan_root), files=contents)
            except OSError as exc:
                emit("ramdisk_contents_error", message=str(exc))
            try:
                unmount(disk.letter, force=True, missing_ok=True)
            except ImDiskError as exc:
                emit("error", message=f"unmount failed: {exc}")
        emit("cleanup_done", ramdisk_unmounted=disk is not None, timestamp=time.time())


def spawn_cli(cli: Path, stderr_path: Path) -> subprocess.Popen[bytes]:
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    handle = stderr_path.open("wb")
    proc = subprocess.Popen(
        [str(cli), "--rpc-stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=handle,
        bufsize=0,
    )
    proc.stderr_handle = handle  # type: ignore[attr-defined]
    return proc


def run_stage3(args: argparse.Namespace) -> int:
    blender = find_blender(args.blender)
    cli_path = find_cli(args.cli)
    job_frame_start = args.frame_start
    job_frame_end = args.frame_end
    scene: dict[str, Any] = {}
    if args.blend and (
        job_frame_start is None
        or job_frame_end is None
        or args.output_width is None
        or args.output_height is None
    ):
        inspect_out = Path(args.export_dir).resolve() / "inspect.json"
        scene = inspect_blend(blender, Path(args.blend), inspect_out)
        if job_frame_start is None:
            job_frame_start = int(scene["frame_start"])
        if job_frame_end is None:
            job_frame_end = int(scene["frame_end"])
        emit("inspect_frames", frame_start=job_frame_start, frame_end=job_frame_end, injected=False)
    if job_frame_start is None:
        job_frame_start = 1
    if job_frame_end is None:
        job_frame_end = job_frame_start
    disk = discover(letter=args.letter, label=args.label)
    ram_dir = disk.root / "dlss"
    ram_dir.mkdir(parents=True, exist_ok=True)
    export_dir = Path(args.export_dir).resolve()
    export_dir.mkdir(parents=True, exist_ok=True)
    if export_dir.drive.upper().rstrip("\\") == f"{disk.letter}:":
        raise ImDiskError("导出目录必须在工作盘，不能写到 RAM 盘")

    total_bytes = disk.total_bytes
    free_bytes = disk.free_bytes
    if total_bytes <= 0:
        total_bytes, free_bytes = volume_space(disk.letter)
    fill_percent = int(getattr(args, "ram_fill_percent", DEFAULT_RAM_FILL_PERCENT))
    reserve_bytes = int(getattr(args, "ram_reserve_bytes", DEFAULT_RAM_RESERVE_BYTES))
    budget = ram_window_budget(total_bytes, fill_percent=fill_percent, reserve_bytes=reserve_bytes)

    out_w = args.output_width or int(scene.get("resolution_x") or 1920)
    out_h = args.output_height or int(scene.get("resolution_y") or 1080)
    native = bool(getattr(args, "native_res_buffers", False))
    pct = int(args.percentage) if args.percentage is not None else int(scene.get("resolution_percentage") or 100)
    dual = wants_dual_pass(native_res_buffers=native, mode=args.mode, percentage=pct)
    render_w = max(1, int(out_w * pct / 100.0))
    render_h = max(1, int(out_h * pct / 100.0))
    bytes_per_frame = estimate_window_frame_bytes(
        render_w, render_h, out_w, out_h, mode=args.mode, dual_pass=dual
    )
    window_frames = getattr(args, "ram_window_frames", None)
    if native:
        windows = plan_ram_windows(
            job_frame_start,
            job_frame_end,
            bytes_per_frame,
            budget,
            max_frames=window_frames,
        )
    else:
        windows = [(job_frame_start, job_frame_end)]
    color_dir = ram_dir / COLOR_SUBDIR
    guides_dir = ram_dir / GUIDES_SUBDIR
    if dual:
        color_dir.mkdir(parents=True, exist_ok=True)
        guides_dir.mkdir(parents=True, exist_ok=True)

    blender_proc: subprocess.Popen[str] | None = None
    cli_proc: subprocess.Popen[bytes] | None = None
    rpc: StdioRpcClient | None = None
    peak_bytes = 0
    peak_files = 0
    stop_requested = False
    blender_exit: int | None = None

    def handle_stop(_signum: int, _frame: object) -> None:
        nonlocal stop_requested
        stop_requested = True
        emit("signal", message="收到停止：不卸载内存盘")
        kill_tree(blender_proc)
        if rpc is not None:
            try:
                rpc.request("stop", {})
            except Exception:
                pass

    if hasattr(signal, "SIGINT"):
        signal.signal(signal.SIGINT, handle_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, handle_stop)

    try:
        emit(
            "stage3_start",
            message=(
                "原生分辨率缓冲：按窗口双程"
                if dual
                else (
                    "原生分辨率缓冲：一趟输出分辨率"
                    if native
                    else "质量百分比一趟进 DLSS"
                )
            ),
            ramdisk=str(disk.root),
            letter=disk.letter,
            label=args.label,
            export_dir=str(export_dir),
            mode=args.mode,
            cli=str(cli_path),
            will_unmount=False,
            percentage=pct,
            native_res_buffers=native,
            dual_pass=dual,
            exr_color_depth=EXR_COLOR_DEPTH,
            ram_fill_percent=fill_percent,
            ram_reserve_bytes=reserve_bytes,
            ram_total_bytes=total_bytes,
            ram_free_bytes=free_bytes,
            ram_budget_bytes=budget,
            estimated_exr_bytes=bytes_per_frame,
            windows=[{"start": a, "end": b} for a, b in windows],
            window_count=len(windows),
        )
        if native:
            emit(
                "ram_window_plan",
                message=(
                    f"内存盘 {disk.letter}: 容量 {format_bytes(total_bytes)}，"
                    f"预算 {format_bytes(budget)}（{fill_percent}% 或留 {format_bytes(reserve_bytes)}），"
                    f"估计每帧 {format_bytes(bytes_per_frame)}，共 {len(windows)} 个窗口"
                    + ("，每窗 Beauty+Guides" if dual else "")
                ),
                letter=disk.letter,
                total_bytes=total_bytes,
                budget_bytes=budget,
                bytes_per_frame=bytes_per_frame,
                windows=len(windows),
                frame_start=job_frame_start,
                frame_end=job_frame_end,
                render_w=render_w,
                render_h=render_h,
                output_w=out_w,
                output_h=out_h,
                dual_pass=dual,
            )

        if args.dry_run:
            cmd = build_blender_cmd(
                args,
                blender,
                ram_dir,
                frame_start=job_frame_start,
                frame_end=job_frame_end,
                dry_run=True,
                config_frame_start=job_frame_start,
                config_frame_end=job_frame_end,
            )
            emit("blender_start", message="dry-run：只写 dlssconfig / LUT，不渲染", cmd=cmd)
            blender_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
            code, peak_b, peak_f, _measured = pump_blender(
                blender_proc, ram_dir, budget_bytes=budget, stop_flag=lambda: stop_requested
            )
            blender_proc = None
            emit("blender_exit", code=code)
            emit(
                "ramdisk_peak",
                message="dry-run 完成，未卸载内存盘",
                bytes=peak_b,
                files=peak_f,
                remaining_files=[],
                remaining_bytes=0,
                letter=disk.letter,
                budget_bytes=budget,
            )
            return code

        # start_job loads dlssconfig immediately. Bake it (and LUTs) onto the
        # RAM disk before spawning the engine, then skip-config on the render pass.
        config_cmd = build_blender_cmd(
            args,
            blender,
            ram_dir,
            frame_start=job_frame_start,
            frame_end=job_frame_end,
            dry_run=True,
            config_frame_start=job_frame_start,
            config_frame_end=job_frame_end,
            config_dir=ram_dir,
        )
        emit("blender_start", message="写 dlssconfig / LUT，不渲染", cmd=config_cmd)
        blender_proc = subprocess.Popen(
            config_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        config_code, _peak_b, _peak_f, _measured = pump_blender(
            blender_proc, ram_dir, budget_bytes=budget, stop_flag=lambda: stop_requested
        )
        blender_proc = None
        emit("blender_exit", code=config_code, phase="config")
        if config_code not in (0, None):
            emit("error", message=f"写 dlssconfig 失败，退出码 {config_code}")
            return int(config_code or 1)
        if not (ram_dir / "dlssconfig.json").is_file():
            emit("error", message="inject 未写出 dlssconfig.json")
            return 1

        stderr_path = export_dir / "cli.stderr.log"
        cli_proc = spawn_cli(cli_path, stderr_path)
        rpc = StdioRpcClient(cli_proc)
        emit("cli_pid", pid=cli_proc.pid, stderr=str(stderr_path))

        try:
            output_template = resolve_stage3_template(args)
        except ValueError as exc:
            emit("error", message=str(exc))
            print(str(exc), file=sys.stderr)
            return 1
        job_params: dict[str, Any] = {
            "mode": args.mode,
            "input_dir": str(ram_dir),
            "output_dir": str(export_dir),
            "output_format": args.output_format,
            "output_template": output_template,
            "frame_start": job_frame_start,
            "frame_end": job_frame_end,
            "window": args.window,
            "watch": True,
            "wait_for_appearance": True,
            "delete_consumed": True,
            "native_res_buffers": native,
            "config": str(ram_dir / "dlssconfig.json"),
            "overwrite_mode": "overwrite",
            "rr_preset": str(getattr(args, "rr_preset", "F") or "F"),
        }
        if dual:
            job_params["color_dir"] = str(color_dir)
            job_params["guides_dir"] = str(guides_dir)
        fg = parse_fg_multiplier(getattr(args, "fg", None))
        if fg >= 2:
            job_params["fg_multiplier"] = fg
        job = rpc.request("start_job", job_params)
        emit("job_started", message="引擎已启动，等待 EXR", **job)

        def run_blender_pass(
            *,
            output_dir: Path,
            win_start: int,
            win_end: int,
            buffer_pass: str,
            skip_config: bool,
            window_index: int,
            percentage: int | None = None,
        ) -> tuple[int, int]:
            nonlocal blender_proc, blender_exit, peak_bytes, peak_files
            cmd = build_blender_cmd(
                args,
                blender,
                output_dir,
                frame_start=win_start,
                frame_end=win_end,
                dry_run=False,
                skip_config=skip_config,
                config_frame_start=job_frame_start,
                config_frame_end=job_frame_end,
                buffer_pass=buffer_pass,
                percentage=percentage,
                config_dir=ram_dir,
            )
            emit("blender_start", cmd=cmd, output_dir=str(output_dir), window=window_index, pass_name=buffer_pass)
            blender_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
            emit("blender_pid", pid=blender_proc.pid, window=window_index, pass_name=buffer_pass)
            code, win_peak, win_files, measured = pump_blender(
                blender_proc,
                ram_dir,
                budget_bytes=budget,
                rpc=rpc,
                stop_flag=lambda: stop_requested,
            )
            blender_exit = code
            peak_bytes = max(peak_bytes, win_peak)
            peak_files = max(peak_files, win_files)
            blender_proc = None
            emit("blender_exit", code=code, window=window_index, pass_name=buffer_pass)
            return code, measured

        planned = list(windows)
        index = 0
        windows_incomplete = False
        wrote_config = True
        while index < len(planned):
            if stop_requested:
                break
            win_start, win_end = planned[index]
            emit(
                "ram_window_start",
                message=f"窗口 {index + 1}/{len(planned)}：{phase_label('combined', win_start, win_end)}",
                index=index + 1,
                total=len(planned),
                frame_start=win_start,
                frame_end=win_end,
                budget_bytes=budget,
            )
            if dual:
                emit(
                    "job_phase",
                    message=phase_label("beauty", win_start, win_end),
                    phase="beauty",
                    frame_start=win_start,
                    frame_end=win_end,
                )
                code, measured = run_blender_pass(
                    output_dir=color_dir,
                    win_start=win_start,
                    win_end=win_end,
                    buffer_pass="beauty",
                    skip_config=wrote_config,
                    window_index=index + 1,
                    percentage=pct,
                )
                if code not in (0, None):
                    emit("error", message=f"Beauty 趟失败，窗口 {index + 1} 退出码 {code}")
                    break
                wrote_config = True
                emit(
                    "job_phase",
                    message=phase_label("guides", win_start, win_end),
                    phase="guides",
                    frame_start=win_start,
                    frame_end=win_end,
                )
                code, _guide_measured = run_blender_pass(
                    output_dir=guides_dir,
                    win_start=win_start,
                    win_end=win_end,
                    buffer_pass="guides",
                    skip_config=True,
                    window_index=index + 1,
                    percentage=100,
                )
                if code not in (0, None):
                    emit("error", message=f"Guides 趟失败，窗口 {index + 1} 退出码 {code}，不用 1-spp Combined 冒充 color")
                    break
            else:
                emit(
                    "job_phase",
                    message=phase_label("combined", win_start, win_end),
                    phase="combined",
                    frame_start=win_start,
                    frame_end=win_end,
                )
                code, measured = run_blender_pass(
                    output_dir=ram_dir,
                    win_start=win_start,
                    win_end=win_end,
                    buffer_pass="combined",
                    skip_config=wrote_config,
                    window_index=index + 1,
                    percentage=pct if native else None,
                )
                if code not in (0, None):
                    emit("error", message=f"Blender 窗口 {index + 1} 退出码 {code}")
                    break
                wrote_config = True

            emit(
                "job_phase",
                message=phase_label("rebuild", win_start, win_end),
                phase="rebuild",
                frame_start=win_start,
                frame_end=win_end,
            )
            leftover, leftover_bytes = wait_exrs_consumed(
                ram_dir,
                rpc,
                timeout=max(30.0, float(args.job_timeout)),
                stop_flag=lambda: stop_requested,
            )
            peak_bytes = max(peak_bytes, leftover_bytes)
            emit(
                "ram_window_consumed",
                message=(
                    f"窗口 {index + 1} 已消费并删除 EXR"
                    if not leftover
                    else f"窗口 {index + 1} 仍有 {len(leftover)} 个 EXR"
                ),
                index=index + 1,
                leftover_files=leftover,
                leftover_bytes=leftover_bytes,
                peak_bytes=peak_bytes,
            )
            if leftover:
                emit(
                    "error",
                    message=f"窗口 {index + 1} 结束后 EXR 未清空，中止后续窗口以免叠盘",
                )
                if index + 1 < len(planned):
                    windows_incomplete = True
                break

            if native and measured > 0 and index + 1 < len(planned):
                bytes_per_frame = measured
                rest_start = win_end + 1
                if rest_start <= job_frame_end:
                    rest = plan_ram_windows(
                        rest_start,
                        job_frame_end,
                        bytes_per_frame,
                        budget,
                        max_frames=window_frames,
                    )
                    planned = planned[: index + 1] + rest
                    emit(
                        "ram_window_replan",
                        message=f"按实测每帧 {format_bytes(measured)} 重排剩余窗口，还有 {len(rest)} 段",
                        bytes_per_frame=measured,
                        remaining=len(rest),
                    )
            index += 1

        if stop_requested and index < len(planned):
            windows_incomplete = True

        leftover, leftover_bytes = wait_exrs_consumed(
            ram_dir,
            rpc,
            timeout=max(30.0, float(args.job_timeout)),
            stop_flag=lambda: stop_requested,
        )
        peak_bytes = max(peak_bytes, leftover_bytes)
        deadline = time.time() + max(30.0, float(args.job_timeout))
        while time.time() < deadline:
            files, used = scan_exrs(ram_dir)
            peak_bytes = max(peak_bytes, used)
            peak_files = max(peak_files, len(files))
            try:
                progress = rpc.request("progress", {})
                emit("engine_progress", **progress, ramdisk_bytes=used, ramdisk_files=len(files))
                state = progress.get("state")
                if state in (2, 3, "done", "error", 0, "idle"):
                    break
            except RpcError as exc:
                emit("rpc_warn", message=str(exc))
                break
            time.sleep(0.25)

        remaining, remaining_bytes = scan_exrs(ram_dir)
        over = peak_bytes > budget
        emit(
            "ramdisk_peak",
            message=(
                f"内存盘峰值 {format_bytes(peak_bytes)} / 预算 {format_bytes(budget)}"
                + ("，超出预算" if over else "，未超预算")
            ),
            bytes=peak_bytes,
            files=peak_files,
            remaining_files=remaining,
            remaining_bytes=remaining_bytes,
            letter=disk.letter,
            budget_bytes=budget,
            over_budget=over,
        )
        try:
            preview = rpc.request("preview_frame", {})
            emit("preview_frame", **preview)
        except RpcError as exc:
            emit("preview_unavailable", message=str(exc))

        if blender_exit not in (None, 0):
            return blender_exit
        if windows_incomplete:
            emit("error", message="内存盘窗口循环未跑完全部帧")
            return 1
        if remaining:
            emit("error", message="任务结束仍有未删除 EXR")
            return 1
        if over and native:
            emit("error", message="内存盘峰值超过 90%/1GB 预算")
            return 1
        return 0
    except ImDiskError as exc:
        emit("error", message=str(exc), missing=exc.missing)
        print(str(exc), file=sys.stderr)
        return 2 if exc.missing else 1
    except (FileNotFoundError, RpcError) as exc:
        emit("error", message=str(exc))
        print(str(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        emit("interrupted")
        return 130
    finally:
        kill_tree(blender_proc)
        if rpc is not None:
            try:
                rpc.request("stop", {})
            except Exception:
                pass
            try:
                rpc.close()
            except Exception:
                pass
        if cli_proc is not None:
            handle = getattr(cli_proc, "stderr_handle", None)
            if cli_proc.poll() is None:
                kill_tree(cli_proc)
            if handle is not None:
                try:
                    handle.close()
                except Exception:
                    pass
        emit(
            "cleanup_done",
            message="清理完成，内存盘保持挂载",
            ramdisk_unmounted=False,
            letter=disk.letter,
            timestamp=time.time(),
        )


def resolve_stage3_template(args: argparse.Namespace) -> str:
    job = args.job_name or job_token_from_blend(args.blend)
    return resolve_filename_template(
        args.output_template,
        job=job,
        mode=args.mode,
        version=getattr(args, "output_version", 1) or 1,
        format=args.output_format,
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="pipeline-v3 RAM disk + Blender inject + engine consume")
    parser.add_argument("--stage", type=int, choices=(2, 3), default=3, help="2=create/unmount, 3=attach existing (default)")
    parser.add_argument("--blend", default=None, help="input .blend; omit for factory-startup smoke")
    parser.add_argument("--blender", default=None, help="path to blender.exe")
    parser.add_argument("--cli", default=None, help="path to dlss-compositor-cli.exe")
    parser.add_argument(
        "--percentage",
        type=int,
        default=None,
        help="SR render resolution_percentage; omit to leave the blend. Ignored for RR (quality slot only).",
    )
    parser.add_argument(
        "--legacy-scale",
        action="store_true",
        help="SR: if --percentage is omitted, use the quality-ratio percentage",
    )
    parser.add_argument(
        "--native-res-buffers",
        action="store_true",
        help="opt-in native-res buffers: SR dual-pass when percentage<100; RR one output-res pass",
    )
    parser.add_argument("--frame-start", type=int, default=None)
    parser.add_argument("--frame-end", type=int, default=None)
    parser.add_argument(
        "--quality",
        default="BALANCED",
        choices=QUALITY_SLOTS,
        help="DLSS quality slot. RR uses this for render scale (no arbitrary %).",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--engine", default=None)
    parser.add_argument("--samples", type=int, default=None)
    parser.add_argument("--use-adaptive-sampling", default=None, help="true|false; omit to leave blend")
    parser.add_argument("--adaptive-threshold", type=float, default=None)
    parser.add_argument("--adaptive-min-samples", type=int, default=None)
    parser.add_argument("--time-limit", type=float, default=None)
    parser.add_argument("--use-denoising", default=None, help="true|false; ignored in RR")
    parser.add_argument("--denoiser", default=None, help="OPENIMAGEDENOISE | OPTIX")
    parser.add_argument("--size", default=DEFAULT_SIZE, help="ImDisk size for Stage 2 only")
    parser.add_argument("--letter", default=None, help="drive letter; Stage 3 default discovers DLSSRAM")
    parser.add_argument("--label", default=VOLUME_LABEL)
    parser.add_argument("--mode", choices=("sr", "rr", "nr"), default="sr")
    parser.add_argument("--fg", default=None, help="Frame Generation multiplier: 2x..6x (SR or RR).")
    parser.add_argument(
        "--rr-preset",
        default="F",
        choices=("D", "E", "F"),
        help="DLSS-RR preset. F = RR2 (310.7.128+ / driver ≥580).",
    )
    parser.add_argument(
        "--sampling-pattern",
        default=None,
        choices=("AUTOMATIC", "TABULATED_SOBOL", "BLUE_NOISE"),
        help="Cycles sampling pattern (RR only). TABULATED_SOBOL ≈ white noise (RR §3.5).",
    )
    parser.add_argument("--export-dir", default="build/stage3-out", help="working-disk output (MOV/sequence)")
    parser.add_argument("--output-format", default="prores_422hq", help="exr | png_16 | prores_422hq")
    parser.add_argument(
        "--output-template",
        default=DEFAULT_FILENAME_TEMPLATE,
        help="filename template; unresolved placeholders fail (no output.mov fallback)",
    )
    parser.add_argument("--job-name", default=None, help="{job} token; default is the blend stem")
    parser.add_argument("--output-version", type=int, default=1, help="{version} token, default 1")
    parser.add_argument("--window", type=int, default=6, help="engine EXR prefetch/delete window, 4-8")
    parser.add_argument(
        "--ram-fill-percent",
        type=int,
        default=DEFAULT_RAM_FILL_PERCENT,
        help="RAM-disk fill cap percent (default 90)",
    )
    parser.add_argument(
        "--ram-reserve",
        default="1G",
        help="RAM-disk reserve to leave free (default 1G)",
    )
    parser.add_argument(
        "--ram-window-frames",
        type=int,
        default=None,
        help="cap frames per RAM-disk write window (tests / force two-plus windows)",
    )
    parser.add_argument("--job-timeout", type=float, default=120.0)
    parser.add_argument("--output-width", type=int, default=None, help="export resolution_x")
    parser.add_argument("--output-height", type=int, default=None, help="export resolution_y")
    parser.add_argument("--fps", type=int, default=None, help="default 30 if the blend has no fps")
    parser.add_argument("--view-transform", default=None, help="OCIO view transform")
    parser.add_argument("--display-device", default=None, help="OCIO display device")
    parser.add_argument("--look", default=None, help="OCIO look")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.window < 4 or args.window > 8:
        emit("error", message="window must be 4-8")
        return 1
    fill = int(args.ram_fill_percent)
    if fill < 1 or fill > 100:
        emit("error", message="ram-fill-percent 必须是 1-100")
        return 1
    try:
        args.ram_reserve_bytes = parse_size(str(args.ram_reserve))
    except (ValueError, IndexError) as exc:
        emit("error", message=f"ram-reserve 无效: {exc}")
        return 1
    if args.ram_window_frames is not None and args.ram_window_frames < 1:
        emit("error", message="ram-window-frames 必须 ≥ 1")
        return 1
    pct, pct_reason = resolve_render_percentage(
        args.percentage,
        args.quality,
        legacy_scale=bool(args.legacy_scale),
        native_res_buffers=bool(args.native_res_buffers),
        mode=args.mode,
    )
    args.percentage = pct
    dual = wants_dual_pass(
        native_res_buffers=bool(args.native_res_buffers),
        mode=args.mode,
        percentage=pct,
    )
    if pct_reason == "blend":
        pct_msg = "沿用 blend 渲染百分比（质量百分比路径）"
    elif pct_reason == "rr_native_output":
        pct_msg = "RR 原生分辨率缓冲：一趟 100%"
    elif pct_reason == "rr_quality_scale":
        pct_msg = f"RR 仅支持质量档：{args.quality} → {pct}%"
    elif pct_reason == "legacy_quality_scale":
        pct_msg = f"按 {args.quality} 降到 {pct}%"
    else:
        pct_msg = f"使用指定渲染百分比 {pct}%"
    if dual:
        pct_msg += "；勾上原生缓冲且未满 100%，每窗 Beauty+Guides"
    elif args.native_res_buffers and pct == 100:
        pct_msg += "；已是 100%，禁止空转第二趟"
    emit(
        "render_percentage",
        message=pct_msg,
        percentage=pct,
        reason=pct_reason,
        quality=args.quality,
        native_res_buffers=bool(args.native_res_buffers),
        dual_pass=dual,
    )
    if args.mode == "nr":
        emit("error", message=NR_RESERVED_MESSAGE)
        print(NR_RESERVED_MESSAGE, file=sys.stderr)
        return 3
    try:
        fg = parse_fg_multiplier(args.fg)
    except ValueError as exc:
        emit("error", message=str(exc))
        print(str(exc), file=sys.stderr)
        return 4
    try:
        policy = cycles_policy_for_mode(args.mode)
    except ValueError as exc:
        emit("error", message=str(exc))
        print(str(exc), file=sys.stderr)
        return 3
    if policy["require_cycles"] and not args.engine:
        args.engine = "CYCLES"
    if args.stage == 2:
        emit("stage2_start", imdisk_note=IMDISK_MISSING_MESSAGE)
        return run_stage2(args)
    try:
        resolve_stage3_template(args)
    except ValueError as exc:
        emit("error", message=str(exc))
        print(str(exc), file=sys.stderr)
        return 1
    emit("stage3_orchestrator", imdisk_note=IMDISK_MISSING_MESSAGE, will_unmount=False)
    return run_stage3(args)


if __name__ == "__main__":
    sys.exit(main())
