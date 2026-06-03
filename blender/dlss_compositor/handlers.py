"""
DLSS Compositor render handlers — transparent resolution management + per-frame data collection.

Handler lifecycle:
1. render_init: save user's resolution_percentage → override with DLSS quality percentage
2. render_pre (per-frame): collect camera data + apply jitter → write per-frame cam.json
3. render_complete OR render_cancel: merge per-frame jsons → write dlssconfig.json → restore percentage
4. load_post (Blender startup): crash recovery → restore stale percentage if found

CRITICAL: resolution_percentage save/override/restore is TRANSPARENT to the user.
The user never sees the percentage change — it happens only during active render.
"""
from __future__ import annotations

import bpy
import os
import json
import math
import logging
from pathlib import Path
from typing import Optional

from bpy.types import Scene

log = logging.getLogger("dlss_compositor.handlers")


def _get_props(scene: Scene) -> Optional[object]:
    """Get DLSS compositor properties, or None if not configured."""
    props = getattr(scene, "dlss_compositor", None)
    if props is None:
        log.debug("_get_props: scene '%s' has no dlss_compositor property", scene.name)
    return props


def _get_quality_percentage(quality_mode: str) -> int:
    """Compute render percentage from quality mode (integer arithmetic)."""
    from .quality_mapping import DLSS_RATIO
    ratio = DLSS_RATIO[quality_mode]
    pct = round(100.0 / ratio)
    log.debug("_get_quality_percentage: quality_mode=%s ratio=%.3f pct=%d", quality_mode, ratio, pct)
    return pct


def _collect_camera_data(scene: Scene, frame_num: int) -> dict:
    """Collect per-frame camera matrix, projection, and jitter data."""
    from .jitter import generate_jitter_sequence

    log.debug("_collect_camera_data: frame=%d scene='%s'", frame_num, scene.name)

    camera = scene.camera
    props = scene.dlss_compositor  # type: ignore[attr-defined]

    matrix_world = [[0.0]*4 for _ in range(4)]
    projection   = [[0.0]*4 for _ in range(4)]
    fov          = 45.0
    aspect_ratio = 1.0
    near_clip    = 0.1
    far_clip     = 100.0

    if camera:
        log.debug("_collect_camera_data: active camera='%s'", camera.name)
        for i in range(4):
            for j in range(4):
                matrix_world[i][j] = camera.matrix_world[i][j]

        if hasattr(camera.data, "angle"):
            fov = math.degrees(camera.data.angle)
        if hasattr(camera.data, "clip_start"):
            near_clip = camera.data.clip_start
        if hasattr(camera.data, "clip_end"):
            far_clip = camera.data.clip_end

        render = scene.render
        if render.resolution_y > 0:
            aspect_ratio = render.resolution_x / render.resolution_y

        proj = camera.calc_matrix_camera(
            bpy.context.evaluated_depsgraph_get(),
            x=render.resolution_x,
            y=render.resolution_y,
            scale_x=render.pixel_aspect_x,
            scale_y=render.pixel_aspect_y,
        )
        for i in range(4):
            for j in range(4):
                projection[i][j] = proj[i][j]
    else:
        log.warning("_collect_camera_data: scene '%s' has no active camera", scene.name)

    sequence = generate_jitter_sequence(props.quality_mode, frame_num + 1)  # type: ignore[attr-defined]
    jitter_x, jitter_y = sequence[frame_num] if sequence else (0.0, 0.0)
    log.debug("_collect_camera_data: jitter=(%.4f, %.4f)", jitter_x, jitter_y)

    return {
        "matrix_world": matrix_world,
        "projection":   projection,
        "fov":          fov,
        "aspect_ratio": aspect_ratio,
        "near_clip":    near_clip,
        "far_clip":     far_clip,
        "jitter_x":     jitter_x,
        "jitter_y":     jitter_y,
    }


def _write_cam_json_atomic(output_dir: str, frame_num: int, data: dict) -> None:
    """Atomically write per-frame cam.json to output_dir/frames/<frame_num:04d>.cam.json"""
    frames_dir = Path(output_dir) / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    cam_path = frames_dir / f"{frame_num:04d}.cam.json"
    tmp_path = str(cam_path) + ".tmp"

    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp_path, str(cam_path))
    log.debug("_write_cam_json_atomic: wrote %s", cam_path)


def dlss_render_init(scene: Scene) -> None:
    """
    Called once at start of entire render job.
    Save user's resolution_percentage → override with DLSS quality value.
    """
    log.info("dlss_render_init called: scene='%s'", scene.name)

    props = _get_props(scene)
    if props is None:
        log.warning("dlss_render_init: no props, skipping")
        return
    if not props.enabled:  # type: ignore[attr-defined]
        log.info("dlss_render_init: DLSS not enabled (props.enabled=False), skipping")
        return

    original_pct = scene.render.resolution_percentage
    scene["_dlss_saved_pct"] = original_pct

    dlss_pct = _get_quality_percentage(props.quality_mode)  # type: ignore[attr-defined]
    scene.render.resolution_percentage = dlss_pct

    log.info(
        "dlss_render_init: ENGAGED — original=%d%% → dlss=%d%% (quality=%s)",
        original_pct, dlss_pct, props.quality_mode,  # type: ignore[attr-defined]
    )


def dlss_render_pre(scene: Scene) -> None:
    """
    Called before each frame renders.
    Collect camera + jitter data → write per-frame .cam.json
    """
    log.debug("dlss_render_pre called: scene='%s' frame=%d", scene.name, scene.frame_current)

    props = _get_props(scene)
    if props is None:
        log.debug("dlss_render_pre: no props, skipping")
        return
    if not props.enabled:  # type: ignore[attr-defined]
        log.debug("dlss_render_pre: not enabled, skipping")
        return
    if "_dlss_saved_pct" not in scene:
        log.warning("dlss_render_pre: _dlss_saved_pct missing — render_init was not called properly")
        return

    frame_num  = scene.frame_current
    output_dir = props.output_dir  # type: ignore[attr-defined]

    if not output_dir:
        log.warning("dlss_render_pre: output_dir not set — skipping per-frame cam.json")
        return

    cam_data = _collect_camera_data(scene, frame_num)
    _write_cam_json_atomic(output_dir, frame_num, cam_data)
    log.info("dlss_render_pre: frame=%d cam.json written to %s", frame_num, output_dir)


def _finalize(scene: Scene) -> None:
    """
    Shared finalize logic for render_complete and render_cancel.
    Merge per-frame jsons → write dlssconfig.json → restore percentage.
    """
    log.info("_finalize called: scene='%s'", scene.name)

    props = _get_props(scene)
    if "_dlss_saved_pct" not in scene:
        log.info("_finalize: no _dlss_saved_pct found, nothing to restore")
        return

    saved_pct = scene["_dlss_saved_pct"]
    log.info("_finalize: restoring resolution_percentage to %d%%", saved_pct)

    if props and props.output_dir:  # type: ignore[attr-defined]
        from .dlssconfig_writer import build_top_level_config, merge_per_frame_jsons, write_dlssconfig
        top_level = build_top_level_config(scene, props)
        frames    = merge_per_frame_jsons(props.output_dir)  # type: ignore[attr-defined]
        log.info("_finalize: merging %d frames into dlssconfig.json", len(frames))
        write_dlssconfig(props.output_dir, top_level, frames)  # type: ignore[attr-defined]
        log.info("_finalize: dlssconfig.json written (%d frames)", len(frames))
    else:
        log.warning("_finalize: output_dir not set, skipping dlssconfig.json write")

    scene.render.resolution_percentage = int(saved_pct)
    del scene["_dlss_saved_pct"]
    log.info("_finalize: restored resolution_percentage to %d%%", saved_pct)


def dlss_render_complete(scene: Scene) -> None:
    """Called when render job completes successfully."""
    log.info("dlss_render_complete called: scene='%s'", scene.name)
    _finalize(scene)


def dlss_render_cancel(scene: Scene) -> None:
    """Called when render job is cancelled/interrupted."""
    log.info("dlss_render_cancel called: scene='%s'", scene.name)
    _finalize(scene)


def dlss_load_post(scene: Scene | None = None) -> None:
    """
    Called when Blender starts or loads a .blend file.
    Crash recovery: if _dlss_saved_pct exists, restore it.
    """
    log.debug("dlss_load_post called")

    target_scene = scene
    if target_scene is None:
        ctx = getattr(bpy, "context", None)
        target_scene = getattr(ctx, "scene", None) if ctx is not None else None

    if target_scene is None:
        log.debug("dlss_load_post: no scene available")
        return

    if "_dlss_saved_pct" in target_scene:
        saved_pct   = target_scene["_dlss_saved_pct"]
        current_pct = target_scene.render.resolution_percentage
        log.warning(
            "dlss_load_post: detected stale saved_pct=%d%% (current=%d%%) — restoring",
            saved_pct, current_pct,
        )
        target_scene.render.resolution_percentage = int(saved_pct)
        del target_scene["_dlss_saved_pct"]
    else:
        log.debug("dlss_load_post: no stale saved_pct found")


def register_handlers() -> None:
    """Register all DLSS render handlers with Blender."""
    log.info("register_handlers: registering render hooks")
    bpy.app.handlers.render_init.append(dlss_render_init)
    bpy.app.handlers.render_pre.append(dlss_render_pre)
    bpy.app.handlers.render_complete.append(dlss_render_complete)
    bpy.app.handlers.render_cancel.append(dlss_render_cancel)
    bpy.app.handlers.load_post.append(dlss_load_post)
    log.info("register_handlers: done")


def unregister_handlers() -> None:
    """Unregister all DLSS render handlers."""
    log.info("unregister_handlers: removing render hooks")
    for handler, func in [
        (bpy.app.handlers.render_init,    dlss_render_init),
        (bpy.app.handlers.render_pre,     dlss_render_pre),
        (bpy.app.handlers.render_complete, dlss_render_complete),
        (bpy.app.handlers.render_cancel,  dlss_render_cancel),
        (bpy.app.handlers.load_post,      dlss_load_post),
    ]:
        if func in handler:
            handler.remove(func)
    log.info("unregister_handlers: done")
