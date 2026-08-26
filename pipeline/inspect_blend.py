"""Read-only Blender scene inspect for pipeline-v3.

Invoked as:

    blender -b <blend> --python inspect_blend.py -- --json-out <path>

Do not pass --factory-startup: user Cycles compute-device prefs (CUDA/OptiX) live
in the Blender user config, not the blend.

Named inspect_blend.py so `python pipeline/ramdisk.py` does not shadow stdlib
inspect. Never saves the blend. Does not insert compositor nodes. Does not use
a member-owned interactive Blender instance — this is a fresh CLI process.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bpy  # noqa: E402

from inspect_scene import (  # noqa: E402
    apply_ocio_config,
    collect_cycles_compute,
    collect_scene_state,
    refresh_cycles_devices,
)


def current_ocio_config() -> object | None:
    try:
        import PyOpenColorIO as ocio  # type: ignore[import-not-found]
    except Exception:
        return None
    getter = getattr(ocio, "GetCurrentConfig", None)
    if getter is None:
        return None
    try:
        return getter()
    except Exception:
        return None

SAVE_OPERATORS = (
    "wm.save_mainfile",
    "wm.save_as_mainfile",
    "wm.save_homefile",
)


def emit(event: str, **fields: object) -> None:
    sys.stdout.write("DLSS_EVENT:" + json.dumps({"event": event, **fields}, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def argv_after_double_dash() -> list[str]:
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def refuse_save(*_args: Any, **_kwargs: Any) -> None:
    raise RuntimeError("pipeline-v3 inspect refuses to save the blend")


def install_save_guard() -> None:
    wm = bpy.ops.wm
    for name in SAVE_OPERATORS:
        try:
            setattr(wm, name, refuse_save)
        except Exception:
            pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="pipeline-v3 scene inspect (never saves)")
    parser.add_argument("--json-out", default=None, help="write inspect JSON here")
    return parser.parse_args(argv_after_double_dash())


def main() -> int:
    install_save_guard()
    args = parse_args()
    scene = bpy.context.scene
    refresh_cycles_devices(bpy.context.preferences)
    payload = collect_scene_state(scene, blend=str(bpy.data.filepath or ""))
    payload = apply_ocio_config(payload, current_ocio_config())
    payload["cycles_compute"] = collect_cycles_compute(scene, bpy.context.preferences)
    if str(payload.get("engine") or "").upper() == "CYCLES":
        cycles = payload.get("cycles") or {}
        compute = payload.get("cycles_compute") or {}
        if str(cycles.get("device") or "") == "GPU" and not str(compute.get("compute_device_type") or ""):
            raise ValueError("Cycles GPU 缺少 compute_device_type")
    payload["is_dirty"] = bool(bpy.data.is_dirty)
    payload["will_save_blend"] = False
    emit("inspect", **payload)
    if args.json_out:
        out = Path(args.json_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        fail = {"ok": False, "error": str(exc), "will_save_blend": False}
        emit("inspect_error", **fail)
        args = parse_args()
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(fail, ensure_ascii=False, indent=2), encoding="utf-8")
        raise
