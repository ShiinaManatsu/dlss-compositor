"""NR/FG tool preview bench — measures the preview pipeline over raw RPC.

Spawns the CLI with debug logging, opens a real media + dlssnrbuffer session,
then times nrtool_preview frames (wall ms + CLI eval_ms + JPEG sizes). Run with
a long edge well above the source resolution to A/B the GPU-downscale path
against the full-res readback.

Usage:
  python tools/nr_preview_bench.py --mov S:\\...v9.mov \
      --buffer S:\\..._dlssbuffer [--frames 40] [--long-edge 1920] \
      [--no-nr] [--no-fg] [--multiplier 2]
"""
from __future__ import annotations

import argparse
import base64
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pipeline"))
from rpc_client import StdioRpcClient  # noqa: E402

EXE = ROOT / "build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mov", required=True)
    ap.add_argument("--buffer", required=True)
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--long-edge", type=int, default=1920)
    ap.add_argument("--no-nr", action="store_true")
    ap.add_argument("--no-fg", action="store_true")
    ap.add_argument("--multiplier", type=int, default=2)
    args = ap.parse_args()

    mov = Path(args.mov)
    buf = Path(args.buffer)
    cam = buf / "dlssconfig.json"
    if not mov.is_file():
        print(f"missing mov: {mov}")
        return 2
    if not cam.is_file():
        print(f"missing dlssconfig: {cam}")
        return 2

    log_path = ROOT / "build" / "nr_preview_bench.stderr.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen(
        [str(EXE), "--rpc-stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=log_path.open("wb"),
        bufsize=0,
        env=dict(os.environ, DLSS_COMPOSITOR_LOG="debug"),
        cwd=str(EXE.parent),
    )
    client = StdioRpcClient(proc)
    try:
        info = client.request("nrtool_open", {
            "color_source": str(mov),
            "buffer_dir": str(buf),
            "camera_config": str(cam),
        }, timeout=120.0)
        print(f"open: {info['width']}x{info['height']} buffer={info['buffer_width']}x"
              f"{info['buffer_height']} frames={info['frames']}")

        base = {
            "nr_enabled": not args.no_nr,
            "nr_guidance": 0,  # All (MV + depth)
            "fg_enabled": not args.no_fg,
            "fg_multiplier": args.multiplier,
            "preview_max_long_edge": args.long_edge,
        }

        # Warmup: first frames compile pipelines + reset temporal state.
        for f in range(min(3, info["frames"] - 1)):
            client.request("nrtool_preview", {"frame": f, **base}, timeout=60.0)
        print("warmup done")

        n = min(args.frames, info["frames"] - 1)
        wall: list[float] = []
        eval_ms: list[float] = []
        jpeg_bytes: list[int] = []
        for f in range(1, n + 1):
            t0 = time.perf_counter()
            r = client.request("nrtool_preview", {"frame": f, **base}, timeout=60.0)
            wall.append((time.perf_counter() - t0) * 1000.0)
            eval_ms.append(r.get("eval_ms", 0.0))
            size = (len(r.get("jpeg_b64", "")) + len(r.get("interp_jpeg_b64", ""))) * 3 // 4
            jpeg_bytes.append(size)

        def stat(v: list[float]) -> str:
            return f"avg {sum(v) / len(v):6.1f}  min {min(v):6.1f}  max {max(v):6.1f}"

        print(f"\nframes={n} long_edge={args.long_edge} nr={not args.no_nr} "
              f"fg={not args.no_fg} mult={args.multiplier}")
        print(f"wall ms  : {stat(wall)}   -> playback {len(wall) * 2 / (sum(wall) / 1000.0):.1f} fps @x{args.multiplier}")
        print(f"eval_ms  : {stat(eval_ms)}")
        print(f"jpeg KB  : {stat([float(b) / 1024.0 for b in jpeg_bytes])}")
    finally:
        try:
            client.close()
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()

    # Surface the CLI's own stage breakdown from the debug log.
    try:
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        marks = [ln for ln in lines if "[NrTool] preview frame" in ln][-6:]
        print("\n--- CLI breakdown (last calls) ---")
        for ln in marks:
            print(ln.strip())
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
