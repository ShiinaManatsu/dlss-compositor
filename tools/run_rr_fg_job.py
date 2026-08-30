"""Run RR+FG ProRes on a 5-frame slice of test-frames/rr."""
from __future__ import annotations

import json
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
OUT_DIR = ROOT / "test-frames/rr/pipeline-out"
LOG = OUT_DIR / "cli.fg.stderr.log"
CFG = IN_DIR / "dlssconfig_fg5.json"


def write_slice_config(n: int = 5) -> None:
    src = json.loads((IN_DIR / "dlssconfig.json").read_text(encoding="utf-8"))
    keys = sorted(src["frames"].keys(), key=lambda k: int(k))[:n]
    src["frames"] = {k: src["frames"][k] for k in keys}
    CFG.write_text(json.dumps(src, indent=2), encoding="utf-8")
    print("slice", keys[0], "..", keys[-1], "n=", len(keys))


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_slice_config(5)
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
        print("status", {k: status[k] for k in ("sr_available", "rr_available", "fg_available", "dlss_rr_version", "dlss_fg_version") if k in status})
        job = rpc.request(
            "start_job",
            {
                "mode": "rr+fg",
                "fg_multiplier": 2,
                "input_dir": str(IN_DIR),
                "output_dir": str(OUT_DIR),
                "config": str(CFG),
                "output_format": "prores",
                "output_template": "rr_fg.mov",
                "overwrite_mode": "overwrite",
                "rr_preset": "F",
            },
            timeout=30.0,
        )
        print("start_job", job)
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
            sig = (state, p.get("current_frame"), p.get("percent"))
            if sig != last:
                print(f"  state={state} frame={p.get('current_frame')}/{p.get('total_frames')} percent={p.get('percent'):.1f} {time.time()-t0:.0f}s")
                last = sig
            if state in (2, 3, "done", "error"):
                print("terminal", p)
                break
            if time.time() - t0 > 1800:
                print("timeout, stopping")
                rpc.request("stop", {})
                break
        return 0 if (last and last[0] in (2, "done")) else 1
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
