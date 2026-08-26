"""Export test frames for DLSS pipeline integration tests.

Usage:
    # Dry-run: capture inject_ready JSON only (no render, no Blender GPU)
    python tools/export_test_frames.py --dry-run --mode rr --output test-frames/rr_inject_ready.json
    python tools/export_test_frames.py --dry-run --mode sr  --output test-frames/sr_inject_ready.json

    # Full export to RAM disk (requires R:\\ mounted and Blender 5.3+)
    python tools/export_test_frames.py --mode rr
    python tools/export_test_frames.py --mode sr

Frame range 300–350 is used by default — animation is intact in this range
(the blend's animation breaks after frame 350).

Blend file: F:/Projects/Blender/Films/TypeAsuka/SnowMix/5.render_material_remaster.blend
RAM disk:   R:\\  (default; override with --ram-disk)
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_BLEND = Path(
    r"F:\Projects\Blender\Films\TypeAsuka\SnowMix\5.render_material_remaster.blend"
)
INJECT_PY = REPO_ROOT / "pipeline" / "inject.py"

BLENDER_CANDIDATES = [
    Path(r"D:\Games\SteamLibrary\steamapps\common\Blender\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.3\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"),
    Path(r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe"),
]


def find_blender(explicit: str | None = None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise FileNotFoundError(f"Blender not found at: {p}")
    env = os.environ.get("BLENDER_EXE")
    if env:
        p = Path(env)
        if p.is_file():
            return p
    for candidate in BLENDER_CANDIDATES:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "Blender not found. Set BLENDER_EXE env var or pass --blender."
    )


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument(
        "--blend",
        default=str(DEFAULT_BLEND),
        help="Path to .blend file (default: TypeAsuka SnowMix)",
    )
    p.add_argument(
        "--mode",
        default="rr",
        choices=("rr", "sr"),
        help="Pipeline mode: rr = Cycles+denoising guides, sr = color+depth+mv (default: rr)",
    )
    p.add_argument(
        "--frame-start",
        type=int,
        default=300,
        help="First frame to export (default: 300)",
    )
    p.add_argument(
        "--frame-end",
        type=int,
        default=350,
        help="Last frame to export (default: 350)",
    )
    p.add_argument(
        "--ram-disk",
        default=r"R:\\",
        help="RAM disk root (default: R:\\)",
    )
    p.add_argument(
        "--quality",
        default="QUALITY",
        help="DLSS quality preset for jitter table (default: QUALITY)",
    )
    p.add_argument(
        "--samples",
        type=int,
        default=None,
        help="Override Cycles max samples (default: keep blend value)",
    )
    p.add_argument(
        "--sampling-pattern",
        default="TABULATED_SOBOL",
        choices=("AUTOMATIC", "TABULATED_SOBOL", "BLUE_NOISE"),
        help=(
            "Cycles sampling pattern. TABULATED_SOBOL (white-noise-like) is recommended "
            "by DLSS-RR §3.5 for best temporal stability (default: TABULATED_SOBOL)"
        ),
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Build compositor nodes and dump inject_ready JSON; do not render",
    )
    p.add_argument(
        "--output",
        default=None,
        help="For --dry-run: write captured inject_ready JSON to this path",
    )
    p.add_argument(
        "--blender",
        default=None,
        help="Path to blender.exe (auto-detected if omitted)",
    )
    return p.parse_args()


def run_inject(
    *,
    blender: Path,
    blend: Path,
    output_dir: Path,
    mode: str,
    frame_start: int,
    frame_end: int,
    quality: str,
    samples: int | None,
    sampling_pattern: str,
    dry_run: bool,
) -> list[dict]:
    """Spawn blender -b and collect DLSS_EVENT: lines from stdout."""
    cmd = [
        str(blender),
        "-b",
        str(blend),
        "--python",
        str(INJECT_PY),
        "--",
        "--output-dir",
        str(output_dir),
        "--mode",
        mode,
        "--frame-start",
        str(frame_start),
        "--frame-end",
        str(frame_end),
        "--quality",
        quality,
        "--sampling-pattern",
        sampling_pattern,
    ]
    if samples is not None:
        cmd.extend(["--samples", str(samples)])
    if dry_run:
        cmd.append("--dry-run")

    print(f"[export_test_frames] Spawning: {' '.join(cmd[:6])} ... (inject.py args follow --)")
    print(f"[export_test_frames] Output dir: {output_dir}")

    events: list[dict] = []
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert proc.stdout is not None

    for line in proc.stdout:
        line = line.rstrip()
        if line.startswith("DLSS_EVENT:"):
            try:
                event = json.loads(line[len("DLSS_EVENT:"):])
                events.append(event)
                name = event.get("event", "?")
                if name == "progress":
                    frame = event.get("frame", "?")
                    total = event.get("total", "?")
                    print(f"  frame {frame}/{total}", end="\r", flush=True)
                elif name == "error":
                    print(f"\n[ERROR] {event.get('message', event)}", flush=True)
                elif name in ("inject_start", "inject_ready", "complete", "dry_run_complete", "lut_baked"):
                    print(f"  [{name}]", flush=True)
            except json.JSONDecodeError:
                print(f"  [bad event] {line}", flush=True)
        elif line.strip():
            # Blender verbose output — print at lower priority
            if any(k in line for k in ("Error", "Warning", "Traceback", "DLSS")):
                print(f"  blender: {line}", flush=True)

    proc.wait()
    _, stderr = proc.communicate()
    if proc.returncode != 0:
        print(f"\n[export_test_frames] blender exited {proc.returncode}", flush=True)
        if stderr:
            print(stderr[-2000:], flush=True)

    return events


def main() -> int:
    args = parse_args()
    blend = Path(args.blend)
    if not blend.is_file():
        print(f"[ERROR] Blend file not found: {blend}")
        return 1

    blender = find_blender(args.blender)
    print(f"[export_test_frames] Blender: {blender}")
    print(f"[export_test_frames] Blend:   {blend}")
    print(f"[export_test_frames] Mode:    {args.mode}")
    print(f"[export_test_frames] Frames:  {args.frame_start}–{args.frame_end}")
    print(f"[export_test_frames] Sampling: {args.sampling_pattern}")
    if args.dry_run:
        print("[export_test_frames] DRY RUN — compositor nodes only, no render")

    subdir = f"{args.mode}-test"
    output_dir = Path(args.ram_disk) / subdir
    output_dir.mkdir(parents=True, exist_ok=True)

    events = run_inject(
        blender=blender,
        blend=blend,
        output_dir=output_dir,
        mode=args.mode,
        frame_start=args.frame_start,
        frame_end=args.frame_end,
        quality=args.quality,
        samples=args.samples,
        sampling_pattern=args.sampling_pattern,
        dry_run=args.dry_run,
    )

    print()

    # Find inject_ready (or dry_run_complete) event
    inject_ready = next((e for e in events if e.get("event") == "inject_ready"), None)
    dry_complete = next((e for e in events if e.get("event") == "dry_run_complete"), None)
    errors = [e for e in events if e.get("event") == "error"]

    if errors:
        print(f"[export_test_frames] {len(errors)} error(s):")
        for err in errors:
            print(f"  {err.get('message', err)}")

    if args.dry_run:
        if inject_ready is None:
            print("[export_test_frames] No inject_ready event captured — check errors above")
            return 1
        # Write the inject_ready event to the output file
        out_path = Path(args.output) if args.output else None
        if out_path:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with out_path.open("w", encoding="utf-8") as f:
                json.dump(inject_ready, f, indent=2, ensure_ascii=False)
            print(f"[export_test_frames] Wrote inject_ready -> {out_path}")
        else:
            print(json.dumps(inject_ready, indent=2, ensure_ascii=False))
        return 0

    complete = next((e for e in events if e.get("event") == "complete"), None)
    if complete:
        out = complete.get("output_dir", "")
        print(f"[export_test_frames] Complete. Output: {out}")
        print(f"[export_test_frames] Frames in: {output_dir}")
    else:
        print("[export_test_frames] No 'complete' event — render may have failed")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
