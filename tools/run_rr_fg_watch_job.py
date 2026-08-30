"""RR+FG ProRes via FileWatcher: copy existing frames into a watch dir, consume/delete.

Does not touch the original test-frames/rr EXRs. RAM disk is not used here.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pipeline"))
from rpc_client import StdioRpcClient, RpcError  # noqa: E402
import subprocess

EXE = ROOT / "build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"
SRC_DIR = ROOT / "test-frames/rr"
WATCH_DIR = ROOT / "test-frames/rr/watch-in"
OUT_DIR = ROOT / "test-frames/rr/pipeline-out"
LOG = OUT_DIR / "cli.fg.watch.stderr.log"
# Config must live next to luts/ so relative tonemap_lut resolves.
CFG = SRC_DIR / "dlssconfig_fg5_watch.json"
N = 5
KEYS = [f"{i:04d}" for i in range(300, 300 + N)]


def write_slice_config() -> None:
    src = json.loads((SRC_DIR / "dlssconfig.json").read_text(encoding="utf-8"))
    src["frames"] = {k: src["frames"][k] for k in KEYS}
    CFG.write_text(json.dumps(src, indent=2), encoding="utf-8")
    print("slice", KEYS[0], "..", KEYS[-1], "n=", len(KEYS))


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if WATCH_DIR.exists():
        shutil.rmtree(WATCH_DIR)
    WATCH_DIR.mkdir(parents=True)
    write_slice_config()

    env = dict(os.environ, DLSS_COMPOSITOR_LOG="info")
    proc = subprocess.Popen(
        [str(EXE), "--rpc-stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=LOG.open("wb"),
        bufsize=0,
        env=env,
        cwd=str(EXE.parent),
    )
    rpc = StdioRpcClient(proc)
    try:
        status = rpc.request("getStatus", timeout=60.0)
        print("status", {k: status[k] for k in (
            "sr_available", "rr_available", "fg_available",
            "dlss_rr_version", "dlss_fg_version") if k in status})
        job = rpc.request(
            "start_job",
            {
                "mode": "rr+fg",
                "fg_multiplier": 2,
                "input_dir": str(WATCH_DIR),
                "output_dir": str(OUT_DIR),
                "config": str(CFG),
                "output_format": "prores",
                "output_template": "rr_fg_watch.mov",
                "overwrite_mode": "overwrite",
                "rr_preset": "F",
                "watch": True,
            },
            timeout=30.0,
        )
        print("start_job", job)

        time.sleep(2.0)
        for k in KEYS:
            src = SRC_DIR / f"{k}.exr"
            if not src.exists():
                print("missing source", src)
                return 1
            shutil.copy2(src, WATCH_DIR / f"{k}.exr")
            print("copied", k)
            time.sleep(0.15)

        t0 = time.time()
        last = None
        while True:
            time.sleep(1.0)
            try:
                p = rpc.request("progress", timeout=10.0)
            except RpcError as e:
                print("progress error", e)
                break
            state = p.get("state")
            sig = (state, p.get("current_frame"), p.get("percent"), p.get("total_frames"))
            if sig != last:
                print(
                    f"  state={state} frame={p.get('current_frame')}/{p.get('total_frames')}"
                    f" percent={p.get('percent'):.1f} {time.time()-t0:.0f}s"
                )
                last = sig
            if state in (2, 3, "done", "error"):
                print("terminal", p)
                break
            if time.time() - t0 > 1800:
                print("timeout, stopping")
                rpc.request("stop", {})
                break

        remaining = list(WATCH_DIR.glob("*.exr"))
        originals = [k for k in KEYS if (SRC_DIR / f"{k}.exr").exists()]
        out = OUT_DIR / "rr_fg_watch.mov"
        print("watch leftover", [p.name for p in remaining])
        print("originals intact", len(originals), "/", len(KEYS))
        print("output", out.exists(), out.stat().st_size if out.exists() else 0)
        ok = (
            last
            and last[0] in (2, "done")
            and not remaining
            and len(originals) == len(KEYS)
            and out.exists()
        )
        return 0 if ok else 1
    finally:
        try:
            rpc.request("stop", timeout=5.0)
        except Exception:
            pass
        rpc.close()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("cli exit", proc.returncode)


if __name__ == "__main__":
    sys.exit(main())
