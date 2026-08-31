"""End-to-end R:→S: smoke over CLI JSON-RPC (2026-08-31).

Mirrors the packaged-GUI flow at RPC level:
  A. RR watch-mode export from the RAM disk (ProRes + dlssnrbuffer + live preview)
  B. NR tool consumes the mov + dlssnrbuffer → previews → png_16 export
  C. SR watch-mode export (ProRes)

Usage: python tools/run_e2e_rs_smoke.py [--cli EXE] [--input R:\\dlss] [--out-root S:\\e2e] [--stages abc]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pipeline"))
from rpc_client import StdioRpcClient  # noqa: E402

EXE = ROOT / "build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"
FRAMES_SRC = ROOT / "test-frames/rr"
FRAME_NAMES = ("0300.exr", "0301.exr", "0302.exr")


def seed_input(input_dir: Path) -> None:
    """Copy the 3-frame test set (+ luts + trimmed config) into the input dir."""
    input_dir.mkdir(parents=True, exist_ok=True)
    for name in FRAME_NAMES:
        shutil.copy2(FRAMES_SRC / name, input_dir / name)
    if not (input_dir / "luts").is_dir():
        shutil.copytree(FRAMES_SRC / "luts", input_dir / "luts")
    cfg = json.loads((FRAMES_SRC / "dlssconfig.json").read_text(encoding="utf-8"))
    keep = {n.split(".")[0] for n in FRAME_NAMES}
    cfg["frames"] = {k: v for k, v in cfg["frames"].items() if k in keep}
    (input_dir / "dlssconfig.json").write_text(json.dumps(cfg), encoding="utf-8")


def spawn_cli(cli: Path, log_path: Path) -> subprocess.Popen[bytes]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    return subprocess.Popen(
        [str(cli), "--rpc-stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=log_path.open("wb"),
        bufsize=0,
        env=dict(os.environ, DLSS_COMPOSITOR_LOG="info"),
        cwd=str(cli.parent),
    )


def settle(client: StdioRpcClient, want=(2, 3), timeout_s: float = 180.0) -> dict:
    final: dict = {}
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        final = client.request("progress", timeout=30.0)
        if final.get("state") in want:
            break
        time.sleep(1.0)
    return final


def log_tail(path: Path, keywords: tuple[str, ...]) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    return [ln.strip() for ln in lines if any(k in ln for k in keywords)][-12:]


def shutdown(client: StdioRpcClient, proc: subprocess.Popen[bytes]) -> None:
    try:
        client.request("stop", timeout=10.0)
    except Exception:
        pass
    time.sleep(1.0)
    try:
        proc.stdin.close()
        proc.wait(timeout=15)
    except Exception:
        proc.kill()


def stage_a_rr_nrbuffer(cli: Path, input_dir: Path, out_dir: Path) -> bool:
    print("=== Stage A: RR + nr_buffer + preview (watch, delete_consumed) ===")
    log = out_dir / "cli.stderr.log"
    proc = spawn_cli(cli, log)
    client = StdioRpcClient(proc)
    ok = False
    try:
        client.request("start_job", {
            "input_dir": str(input_dir),
            "config": str(input_dir / "dlssconfig.json"),
            "output_dir": str(out_dir),
            "mode": "rr",
            "output_format": "prores_422hq",
            "output_template": "e2e_rr_v1.mov",
            "rr_preset": "F",
            "watch": True,
            "wait_for_appearance": True,
            "delete_consumed": True,
            "nr_buffer": True,
            "window": 4,
            "overwrite_mode": "overwrite",
        }, timeout=120.0)
        time.sleep(2.0)
        preview = client.request("preview_frame", timeout=30.0)
        jpeg = preview.get("jpeg_b64") or ""
        print(f"  preview_frame ok={preview.get('ok')} jpeg_bytes={len(jpeg) * 3 // 4}")
        final = settle(client)
        print(f"  final -> {json.dumps(final)[:200]}")
        produced = sorted(p.name for p in out_dir.iterdir() if p.is_file())
        mov = [f for f in produced if f.endswith(".mov")]
        buffers = [f for f in produced if f.startswith("dlssnrbuffer_")]
        print(f"  output: {produced}")
        ok = (final.get("state") == 2 and bool(mov) and len(buffers) == len(FRAME_NAMES)
              and preview.get("ok") and len(jpeg) > 1000)
        for ln in log_tail(log, ("error", "Pipeline", "DLSS RR")):
            print(f"    {ln}")
    finally:
        shutdown(client, proc)
    print(f"  STAGE A: {'OK' if ok else 'FAIL'}")
    return ok


def stage_b_nrtool(cli: Path, rr_out: Path, nr_out: Path) -> bool:
    print("=== Stage B: NR tool consumes mov + dlssnrbuffer ===")
    log = nr_out / "cli.stderr.log"
    proc = spawn_cli(cli, log)
    client = StdioRpcClient(proc)
    ok = False
    try:
        info = client.request("nrtool_open", {
            "color_source": str(rr_out / "e2e_rr_v1.mov"),
            "buffer_dir": str(rr_out),
        }, timeout=120.0)
        print(f"  nrtool_open ok={info.get('ok')} frames={info.get('frames')} "
              f"{info.get('width')}x{info.get('height')} err={info.get('error')}")
        all_previews = True
        for frame in range(info.get("frames", 0)):
            prev = client.request("nrtool_preview", {
                "frame": frame,
                "nr_style": 2, "nr_intensity": 1.0,
                "nr_local_tone": 1.0, "nr_local_structure": 0.45,
                "nr_automask": False, "nr_guidance": 0,
            }, timeout=120.0)
            jpeg = prev.get("jpeg_b64") or ""
            print(f"  preview frame={frame} ok={prev.get('ok')} eval_ms={prev.get('eval_ms')} "
                  f"reset={prev.get('reset_applied')} jpeg_bytes={len(jpeg) * 3 // 4}")
            all_previews = all_previews and prev.get("ok") and len(jpeg) > 1000
        client.request("nrtool_export", {
            "output_format": "png_16",
            "output_dir": str(nr_out),
            "output_template": "nr_####.png",
        }, timeout=30.0)
        final = settle(client)
        print(f"  final -> {json.dumps(final)[:200]}")
        produced = sorted(p.name for p in nr_out.glob("nr_*.png"))
        print(f"  nr pngs: {produced}")
        ok = (all_previews and final.get("state") == 2 and len(produced) == info.get("frames", 0))
        for ln in log_tail(log, ("NR", "error")):
            print(f"    {ln}")
    finally:
        shutdown(client, proc)
    print(f"  STAGE B: {'OK' if ok else 'FAIL'}")
    return ok


def stage_c_sr(cli: Path, input_dir: Path, out_dir: Path) -> bool:
    print("=== Stage C: SR export (watch) ===")
    log = out_dir / "cli.stderr.log"
    proc = spawn_cli(cli, log)
    client = StdioRpcClient(proc)
    ok = False
    try:
        client.request("start_job", {
            "input_dir": str(input_dir),
            "config": str(input_dir / "dlssconfig.json"),
            "output_dir": str(out_dir),
            "mode": "sr",
            "output_format": "prores_422hq",
            "output_template": "e2e_sr_v1.mov",
            "watch": True,
            "wait_for_appearance": True,
            "delete_consumed": True,
            "window": 4,
            "overwrite_mode": "overwrite",
        }, timeout=120.0)
        final = settle(client)
        print(f"  final -> {json.dumps(final)[:200]}")
        produced = [p.name for p in out_dir.glob("*.mov")]
        print(f"  output: {sorted(p.name for p in out_dir.iterdir() if p.is_file())}")
        ok = final.get("state") == 2 and bool(produced)
        for ln in log_tail(log, ("error", "Pipeline")):
            print(f"    {ln}")
    finally:
        shutdown(client, proc)
    print(f"  STAGE C: {'OK' if ok else 'FAIL'}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=EXE)
    parser.add_argument("--input", type=Path, default=Path(r"R:\dlss"))
    parser.add_argument("--out-root", type=Path, default=Path(r"S:\e2e"))
    parser.add_argument("--stages", default="abc", help="subset of a/b/c to run")
    args = parser.parse_args()

    results: dict[str, bool] = {}
    rr_out = args.out_root / "rr-out"
    nr_out = args.out_root / "nr-out"
    sr_out = args.out_root / "sr-out"

    if "a" in args.stages:
        for d in (rr_out, nr_out, sr_out):
            d.mkdir(parents=True, exist_ok=True)
        seed_input(args.input)
        results["A_rr_nrbuffer"] = stage_a_rr_nrbuffer(args.cli, args.input, rr_out)
    if "b" in args.stages:
        nr_out.mkdir(parents=True, exist_ok=True)
        results["B_nrtool"] = stage_b_nrtool(args.cli, rr_out, nr_out)
    if "c" in args.stages:
        sr_out.mkdir(parents=True, exist_ok=True)
        # Stage A consumes (deletes) the input frames — reseed for C.
        seed_input(args.input)
        results["C_sr"] = stage_c_sr(args.cli, args.input, sr_out)

    print(json.dumps(results, indent=1))
    return 0 if results and all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
