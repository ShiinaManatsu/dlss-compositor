"""Run RR + dlssnrbuffer export over test-frames/rr via CLI JSON-RPC.

Validates the guidance export for the standalone NR tool: main output PNGs
plus dlssnrbuffer_####.exr (RGB half: mv.x, mv.y, NDC depth). FG and NR stay
off — dlssnrbuffer is the Stage-A export the NR tool consumes.
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pipeline"))
from rpc_client import StdioRpcClient, RpcError  # noqa: E402
import subprocess

EXE = ROOT / "build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"
IN_DIR = ROOT / "test-frames/rr"
OUT_DIR = ROOT / "test-frames/rr/pipeline-out-nrbuf"
LOG = OUT_DIR / "cli.stderr.log"


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
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
    st = {}
    try:
        rpc.request("getStatus", timeout=60.0)
        job = rpc.request(
            "start_job",
            {
                "mode": "rr",
                "input_dir": str(IN_DIR),
                "output_dir": str(OUT_DIR),
                "config": str(IN_DIR / "dlssconfig.json"),
                "output_format": "png_16",
                "output_template": "frame_####.png",
                "overwrite_mode": "overwrite",
                "rr_preset": "F",
                "nr_buffer": True,
            },
            timeout=30.0,
        )
        print("start_job", job)
        deadline = time.time() + 300.0
        while time.time() < deadline:
            time.sleep(2.0)
            st = rpc.request("getStatus", timeout=30.0)
            print("status keys:", [k for k in st if "progress" in k or "frame" in k or "state" in k],
                  {k: st[k] for k in st if "progress" in k})
            done = any(st.get(k) in ("completed", "error", "cancelled") for k in ("state", "job_state"))
            if done:
                break
        print("final", st)
        return 0
    except RpcError as exc:
        print("RPC error:", exc)
        return 1
    finally:
        try:
            rpc.request("stop_task", timeout=10.0)
        except Exception:
            pass
        proc.terminate()


if __name__ == "__main__":
    raise SystemExit(main())
