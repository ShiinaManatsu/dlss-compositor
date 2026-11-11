"""Read-only Blender scene snapshot + opt-in override application.

No bpy import — duck-typed scene objects, importable outside Blender.
Inspect/inject scripts import this after putting pipeline/ on sys.path.
"""
from __future__ import annotations

from typing import Any

from session_config import cycles_policy_for_mode

DENOISER_LABELS = {
    "OPENIMAGEDENOISE": "OIDN",
    "OPTIX": "OptiX",
}

# Blend-side pins. App-side params (export size, OCIO, mode, MOV) are not here.
OVERRIDE_KEYS = (
    "resolution_percentage",
    "frame_start",
    "frame_end",
    "use_adaptive_sampling",
    "adaptive_threshold",
    "samples",
    "adaptive_min_samples",
    "time_limit",
    "use_denoising",
    "denoiser",
)

_CYCLES_ATTR = {
    "use_adaptive_sampling": "use_adaptive_sampling",
    "adaptive_threshold": "adaptive_threshold",
    "samples": "samples",
    "adaptive_min_samples": "adaptive_min_samples",
    "time_limit": "time_limit",
    "use_denoising": "use_denoising",
    "denoiser": "denoiser",
}


def parse_bool(value: str | bool | int) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return bool(value)
    lower = str(value).strip().lower()
    if lower in ("1", "true", "yes", "on"):
        return True
    if lower in ("0", "false", "no", "off"):
        return False
    raise ValueError(f"invalid bool: {value!r}")


def denoiser_label(code: str | None) -> str:
    if not code:
        return ""
    return DENOISER_LABELS.get(str(code), str(code))


def _iter_nodes(tree: Any) -> list[Any]:
    if tree is None:
        return []
    nodes = getattr(tree, "nodes", None)
    if nodes is None:
        return []
    try:
        return list(nodes)
    except TypeError:
        return []


def existing_file_outputs(scene: Any, *, exclude_label: str | None = None) -> list[dict[str, object]]:
    effects = getattr(scene, "compositor_effects", None)
    if effects is None:
        trees = [getattr(scene, "compositing_node_group", None)]
    else:
        trees = dict.fromkeys(effect.node_group for effect in effects)
    found: list[dict[str, object]] = []
    for tree in trees:
        for node in _iter_nodes(tree):
            if getattr(node, "type", None) != "OUTPUT_FILE" or (
                exclude_label is not None and getattr(node, "label", None) == exclude_label
            ):
                continue
            found.append(
                {
                    "name": str(getattr(node, "name", "")),
                    "label": str(getattr(node, "label", "")),
                    "mute": bool(getattr(node, "mute", False)),
                    "directory": str(getattr(node, "directory", "")),
                    "file_name": str(getattr(node, "file_name", "")),
                }
            )
    return found


def effective_fps(render: Any) -> float:
    fps = float(getattr(render, "fps", 0.0) or 0.0)
    base = float(getattr(render, "fps_base", 1.0) or 1.0)
    if base <= 0.0:
        return fps
    return fps / base


def _view_layer_store_passes(scene: Any) -> bool:
    for view_layer in getattr(scene, "view_layers", []) or []:
        cycles_vl = getattr(view_layer, "cycles", None)
        if cycles_vl is not None and bool(getattr(cycles_vl, "denoising_store_passes", False)):
            return True
    return False


def snapshot_cycles(scene: Any) -> dict[str, Any] | None:
    cycles = getattr(scene, "cycles", None)
    if cycles is None:
        return None
    denoiser = str(getattr(cycles, "denoiser", "") or "")
    preview_denoiser = str(getattr(cycles, "preview_denoiser", "") or "")
    device = str(getattr(cycles, "device", "") or "")
    store = bool(getattr(cycles, "denoising_store_passes", False)) or _view_layer_store_passes(scene)
    sampling_pattern = str(getattr(cycles, "sampling_pattern", "") or "")
    return {
        "use_adaptive_sampling": bool(getattr(cycles, "use_adaptive_sampling", False)),
        "adaptive_threshold": round(float(getattr(cycles, "adaptive_threshold", 0.0) or 0.0), 4),
        "samples": int(getattr(cycles, "samples", 0) or 0),
        "adaptive_min_samples": int(getattr(cycles, "adaptive_min_samples", 0) or 0),
        "time_limit": float(getattr(cycles, "time_limit", 0.0) or 0.0),
        "use_denoising": bool(getattr(cycles, "use_denoising", False)),
        "denoiser": denoiser,
        "denoiser_label": denoiser_label(denoiser),
        "preview_denoiser": preview_denoiser,
        "preview_denoiser_label": denoiser_label(preview_denoiser),
        "denoising_store_passes": store,
        "device": device,
        "sampling_pattern": sampling_pattern,
    }


def refresh_cycles_devices(preferences: Any | None) -> None:
    if preferences is None:
        return
    try:
        addons = getattr(preferences, "addons", None)
        addon = addons.get("cycles") if hasattr(addons, "get") else addons["cycles"]
        prefs = getattr(addon, "preferences", None)
        if prefs is not None and hasattr(prefs, "get_devices"):
            prefs.get_devices()
    except Exception:
        pass


def collect_cycles_compute(scene: Any, preferences: Any | None = None) -> dict[str, Any]:
    """User-pref Cycles devices. Requires a real bpy preferences object for GPU list."""
    cycles = getattr(scene, "cycles", None)
    scene_device = str(getattr(cycles, "device", "") or "") if cycles is not None else ""
    compute_type = ""
    devices: list[dict[str, object]] = []
    if preferences is not None:
        addons = getattr(preferences, "addons", None)
        addon = None
        if addons is not None:
            try:
                addon = addons.get("cycles") if hasattr(addons, "get") else addons["cycles"]
            except Exception:
                addon = None
        prefs = getattr(addon, "preferences", None) if addon is not None else None
        if prefs is not None:
            compute_type = str(getattr(prefs, "compute_device_type", "") or "")
            raw_devices = getattr(prefs, "devices", None)
            try:
                iterable = list(raw_devices) if raw_devices is not None else []
            except TypeError:
                iterable = []
            for dev in iterable:
                devices.append(
                    {
                        "name": str(getattr(dev, "name", "")),
                        "type": str(getattr(dev, "type", "")),
                        "use": bool(getattr(dev, "use", False)),
                    }
                )
    return {
        "scene_device": scene_device,
        "compute_device_type": compute_type,
        "devices": devices,
    }


def snapshot_render(scene: Any) -> dict[str, Any]:
    render = scene.render
    return {
        "resolution_x": int(getattr(render, "resolution_x", 0) or 0),
        "resolution_y": int(getattr(render, "resolution_y", 0) or 0),
        "resolution_percentage": int(getattr(render, "resolution_percentage", 0) or 0),
        "frame_start": int(getattr(scene, "frame_start", 0) or 0),
        "frame_end": int(getattr(scene, "frame_end", 0) or 0),
        "fps": effective_fps(render),
        "engine": str(getattr(render, "engine", "") or ""),
    }


def _enum_ids(owner: Any, prop_name: str) -> list[str]:
    """Read RNA enum identifiers. OCIO view/display lists live here at runtime."""
    bl_rna = getattr(owner, "bl_rna", None)
    props = getattr(bl_rna, "properties", None) if bl_rna is not None else None
    if props is None:
        return []
    prop = None
    try:
        if hasattr(props, "get"):
            prop = props.get(prop_name)
        if prop is None:
            prop = props[prop_name]
    except Exception:
        return []
    items = getattr(prop, "enum_items", None)
    if not items:
        return []
    try:
        iterable = list(items)
    except TypeError:
        return []
    out: list[str] = []
    for item in iterable:
        ident = str(getattr(item, "identifier", "") or "")
        if ident:
            out.append(ident)
    return out


def snapshot_ocio(scene: Any) -> dict[str, Any]:
    """Current view/display plus the legal OCIO items for this blend."""
    view_settings = getattr(scene, "view_settings", None)
    display_settings = getattr(scene, "display_settings", None)
    view_transform = (
        str(getattr(view_settings, "view_transform", "") or "") if view_settings is not None else ""
    )
    display_device = (
        str(getattr(display_settings, "display_device", "") or "") if display_settings is not None else ""
    )
    look = str(getattr(view_settings, "look", "") or "") if view_settings is not None else ""
    view_transforms = _enum_ids(view_settings, "view_transform") if view_settings is not None else []
    display_devices = _enum_ids(display_settings, "display_device") if display_settings is not None else []
    view_transforms = [item for item in view_transforms if item and item != "NONE"]
    display_devices = [item for item in display_devices if item and item != "NONE"]
    if view_transform and view_transform not in view_transforms:
        view_transforms = [view_transform, *view_transforms]
    if display_device and display_device not in display_devices:
        display_devices = [display_device, *display_devices]
    return {
        "view_transform": view_transform,
        "display_device": display_device,
        "look": look,
        "view_transforms": view_transforms,
        "display_devices": display_devices,
        "views_by_display": {},
    }


def apply_ocio_config(payload: dict[str, Any], config: Any | None) -> dict[str, Any]:
    """Replace RNA stubs with the blend's live OCIO display/view lists."""
    if config is None:
        return payload
    display = str(payload.get("display_device") or "")
    try:
        displays = [str(item) for item in config.getDisplays() if str(item) and str(item) != "NONE"]
    except Exception:
        displays = list(payload.get("display_devices") or [])
    views_by_display: dict[str, list[str]] = {}
    for name in displays:
        try:
            views_by_display[name] = [
                str(item) for item in config.getViews(name) if str(item) and str(item) != "NONE"
            ]
        except Exception:
            views_by_display[name] = []
    views = list(views_by_display.get(display) or payload.get("view_transforms") or [])
    view = str(payload.get("view_transform") or "")
    if view and view not in views:
        views = [view, *views]
    if display and display not in displays:
        displays = [display, *displays]
    payload["display_devices"] = displays
    payload["view_transforms"] = views
    payload["views_by_display"] = views_by_display
    return payload


def collect_scene_state(scene: Any, *, blend: str = "") -> dict[str, Any]:
    """JSON-ready inspector payload. Never mutates the scene."""
    render_info = snapshot_render(scene)
    file_outputs = existing_file_outputs(scene)
    cycles = snapshot_cycles(scene)
    ocio = snapshot_ocio(scene)
    engine = str(render_info.get("engine") or "").upper()
    if engine == "CYCLES":
        if cycles is None:
            raise ValueError("Cycles 场景缺少 cycles 数据")
        if not cycles.get("denoiser"):
            raise ValueError("Cycles 场景缺少 denoiser")
        if not cycles.get("device"):
            raise ValueError("Cycles 场景缺少 device")
    return {
        "ok": True,
        "blend": blend,
        **render_info,
        "cycles": cycles,
        "file_output_count": len(file_outputs),
        "file_outputs": file_outputs,
        **ocio,
    }


def apply_scene_overrides(
    scene: Any,
    overrides: dict[str, Any] | None,
    mode: str,
) -> dict[str, Any]:
    """Write only keys present and not None. RR always forces Cycles denoise off.

    Unspecified keys are reported in ``untouched`` and left exactly as loaded.
    """
    spec = {k: v for k, v in (overrides or {}).items() if v is not None}
    written: list[str] = []
    untouched: list[str] = []
    forced: list[str] = []
    ignored: list[str] = []
    before = {"render": snapshot_render(scene), "cycles": snapshot_cycles(scene)}

    render = scene.render
    cycles = getattr(scene, "cycles", None)
    policy = cycles_policy_for_mode(mode)

    if "resolution_percentage" in spec:
        render.resolution_percentage = int(spec["resolution_percentage"])
        written.append("resolution_percentage")
    else:
        untouched.append("resolution_percentage")

    if "frame_start" in spec:
        scene.frame_start = int(spec["frame_start"])
        written.append("frame_start")
    else:
        untouched.append("frame_start")

    if "frame_end" in spec:
        scene.frame_end = int(spec["frame_end"])
        written.append("frame_end")
    else:
        untouched.append("frame_end")

    rr_locks_denoise = bool(policy.get("force_denoising"))
    cycles_keys = (
        "use_adaptive_sampling",
        "adaptive_threshold",
        "samples",
        "adaptive_min_samples",
        "time_limit",
        "use_denoising",
        "denoiser",
    )
    for key in cycles_keys:
        if rr_locks_denoise and key in ("use_denoising", "denoiser"):
            if key in spec:
                ignored.append(key)
            continue
        if key not in spec:
            untouched.append(key)
            continue
        if cycles is None:
            untouched.append(key)
            continue
        attr = _CYCLES_ATTR[key]
        value = spec[key]
        if key in ("use_adaptive_sampling", "use_denoising"):
            value = parse_bool(value) if not isinstance(value, bool) else value
        elif key in ("adaptive_threshold", "time_limit"):
            value = float(value)
        elif key == "denoiser":
            value = str(value)
        else:
            value = int(value)
        setattr(cycles, attr, value)
        written.append(key)

    if policy.get("require_cycles"):
        previous = str(getattr(render, "engine", "") or "")
        if previous != "CYCLES":
            render.engine = "CYCLES"
            forced.append("engine")

    if rr_locks_denoise and cycles is not None:
        cycles.use_denoising = False
        forced.append("use_denoising")
        if bool(policy.get("denoising_store_passes")):
            store_set = False
            if hasattr(cycles, "denoising_store_passes"):
                cycles.denoising_store_passes = True
                store_set = True
            for view_layer in getattr(scene, "view_layers", []) or []:
                cycles_vl = getattr(view_layer, "cycles", None)
                if cycles_vl is not None and hasattr(cycles_vl, "denoising_store_passes"):
                    cycles_vl.denoising_store_passes = True
                    store_set = True
            if store_set:
                forced.append("denoising_store_passes")

    after = {"render": snapshot_render(scene), "cycles": snapshot_cycles(scene)}
    return {
        "written": written,
        "untouched": untouched,
        "forced": forced,
        "ignored": ignored,
        "before": before,
        "after": after,
    }


def inject_override_args(overrides: dict[str, Any] | None) -> list[str]:
    """CLI flags for inject.py. Omit a key entirely when it is not overridden."""
    spec = {k: v for k, v in (overrides or {}).items() if v is not None}
    args: list[str] = []
    mapping = (
        ("resolution_percentage", "--percentage"),
        ("frame_start", "--frame-start"),
        ("frame_end", "--frame-end"),
        ("samples", "--samples"),
        ("adaptive_min_samples", "--adaptive-min-samples"),
        ("denoiser", "--denoiser"),
    )
    for key, flag in mapping:
        if key in spec:
            args.extend([flag, str(spec[key])])
    if "use_adaptive_sampling" in spec:
        args.extend(["--use-adaptive-sampling", "true" if parse_bool(spec["use_adaptive_sampling"]) else "false"])
    if "adaptive_threshold" in spec:
        args.extend(["--adaptive-threshold", str(spec["adaptive_threshold"])])
    if "time_limit" in spec:
        args.extend(["--time-limit", str(spec["time_limit"])])
    if "use_denoising" in spec:
        args.extend(["--use-denoising", "true" if parse_bool(spec["use_denoising"]) else "false"])
    if "sampling_pattern" in spec:
        args.extend(["--sampling-pattern", str(spec["sampling_pattern"])])
    return args
