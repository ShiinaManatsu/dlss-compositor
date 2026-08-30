"""NR tool session smoke: open → preview → export via CLI JSON-RPC.

Stage-B validation on the Stage-A exports in test-frames/rr/pipeline-out-nrbuf
(frame_####.png color + dlssnrbuffer_####.exr guidance). Exercises
nrtool_open / nrtool_preview / nrtool_export / nrtool_close.
"""
from __future__ import annotations

import base64
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pipeline"))
from rpc_client import StdioRpcClient, RpcError  # noqa: E402
import subprocess

EXE = ROOT / "build/x64-windows-msvc-release/Release/dlss-compositor-cli.exe"
SRC = ROOT / "test-frames/rr/pipeline-out-nrbuf"
EXPORT = ROOT / "test-frames/rr/nr-tool-out"
LOG = EXPORT / "cli.stderr.log"

SETTINGS = {
    "nr_style": 2,
    "nr_intensity": 1.0,
    "nr_local_tone": 1.0,
    "nr_local_structure": 0.45,
    "nr_automask": False,
    "nr_guidance": 0,
}


def main() -> int:
    EXPORT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DLSS_COMPOSITOR_LOG="info")
    proc = subprocess.Popen(
        [str(EXE), "--rpc-stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=LOG.open("wb"), bufsize=0, env=env, cwd=str(EXE.parent),
    )
    rpc = StdioRpcClient(proc)
    try:
        info = rpc.request("nrtool_open", {
            "color_source": str(SRC),
            "buffer_dir": str(SRC),
        }, timeout=120.0)
        print("open:", {k: info[k] for k in ("color_kind", "frames", "width", "height",
                                             "buffer_width", "buffer_height", "fps")})

        for frame in (0, 1, 2, 1):  # includes a backward scrub → reset expected
            t0 = time.time()
            r = rpc.request("nrtool_preview", {"frame": frame, **SETTINGS}, timeout=120.0)
            ms = (time.time() - t0) * 1000.0
            jpg = base64.b64decode(r.get("jpeg_b64", ""))
            out = EXPORT / f"preview_f{frame}.jpg"
            out.write_bytes(jpg)
            print(f"preview frame={frame} eval_ms={r['eval_ms']:.0f} wall_ms={ms:.0f} "
                  f"reset={r['reset_applied']} jpeg={len(jpg)}B -> {out.name}")

        job = rpc.request("nrtool_export", {
            "output_format": "prores_422hq",
            "output_path": str(EXPORT / "nr_final.mov"),
            **SETTINGS,
        }, timeout=30.0)
        print("export started:", job)
        deadline = time.time() + 300.0
        while time.time() < deadline:
            time.sleep(2.0)
            pr = rpc.request("progress", {}, timeout=15.0)
            state = pr.get("state")
            print("  progress:", pr.get("percent"), "state:", state)
            if state in (2, 3):
                break
        print("final progress:", pr)
        rpc.request("nrtool_close", {}, timeout=30.0)
        return 0 if pr.get("state") == 2 else 1
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
