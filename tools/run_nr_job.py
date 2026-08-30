"""Run RR+NR (feature 18) pipeline over test-frames/rr via CLI JSON-RPC.

Experimental DLSSNR smoke: same as run_rr_job.py but enables kNR and the
DLSSNR tuning params. Requires nvngx_dlssnr.dll next to the CLI exe.
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
OUT_DIR = ROOT / "test-frames/rr/pipeline-out-nr"
LOG = ROOT / "test-frames/rr/pipeline-out-nr/cli.stderr.log"


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
    try:
        status = rpc.request("getStatus", timeout=60.0)
        print("status", {k: status[k] for k in ("sr_available", "rr_available", "fg_available", "dlss_rr_version")})
        job = rpc.request(
            "start_job",
            {
                "mode": "rr",
                "input_dir": str(IN_DIR),
                "output_dir": str(OUT_DIR),
                "config": str(IN_DIR / "dlssconfig.json"),
                "output_format": "png_16",
                "output_template": "nr_####.png",
                "overwrite_mode": "overwrite",
                "rr_preset": "F",
                "nr": True,
                "nr_style": 0,
                "nr_intensity": 1.0,
                "nr_guidance": 0,
            },
            timeout=30.0,
        )
        print("start_job", job)
        # Wait for completion (poll getStatus).
        deadline = time.time() + 300.0
        while time.time() < deadline:
            time.sleep(2.0)
            st = rpc.request("getStatus", timeout=30.0)
            state = st.get("state")
            print("state", state, st.get("current_frame"), "/", st.get("total_frames"))
            if state in ("completed", "error", "cancelled", 3, 4, 5):
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
