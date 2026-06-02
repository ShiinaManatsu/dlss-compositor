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
import bpy
import os
import json
import math
import logging
from pathlib import Path
from typing import Optional

log = logging.getLogger(__name__)


def _get_props(scene) -> Optional[object]:
    """Get DLSS compositor properties, or None if not configured."""
    return getattr(scene, 'dlss_compositor', None)


def _get_quality_percentage(quality_mode: str) -> int:
    """Compute render percentage from quality mode (integer arithmetic)."""
    from .quality_mapping import DLSS_RATIO
    ratio = DLSS_RATIO[quality_mode]
    return round(100.0 / ratio)


def _collect_camera_data(scene, frame_num: int) -> dict:
    """Collect per-frame camera matrix, projection, and jitter data."""
    from .jitter import generate_jitter_sequence

    camera = scene.camera
    props = scene.dlss_compositor

    # Get camera matrices
    matrix_world = [[0.0]*4 for _ in range(4)]
    projection = [[0.0]*4 for _ in range(4)]
    fov = 45.0
    aspect_ratio = 1.0
    near_clip = 0.1
    far_clip = 100.0

    if camera:
        for i in range(4):
            for j in range(4):
                matrix_world[i][j] = camera.matrix_world[i][j]

        if hasattr(camera.data, 'angle'):
            fov = math.degrees(camera.data.angle)
        if hasattr(camera.data, 'clip_start'):
            near_clip = camera.data.clip_start
        if hasattr(camera.data, 'clip_end'):
            far_clip = camera.data.clip_end

        render = scene.render
        if render.resolution_y > 0:
            aspect_ratio = render.resolution_x / render.resolution_y

        # Compute projection matrix
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

    # Compute jitter for this frame
    # Use frame_num as index within the quality's phase sequence
    sequence = generate_jitter_sequence(props.quality_mode, frame_num + 1)
    jitter_x, jitter_y = sequence[frame_num] if sequence else (0.0, 0.0)

    return {
        'matrix_world': matrix_world,
        'projection': projection,
        'fov': fov,
        'aspect_ratio': aspect_ratio,
        'near_clip': near_clip,
        'far_clip': far_clip,
        'jitter_x': jitter_x,
        'jitter_y': jitter_y,
    }


def _write_cam_json_atomic(output_dir: str, frame_num: int, data: dict) -> None:
    """Atomically write per-frame cam.json to output_dir/frames/<frame_num:04d>.cam.json"""
    frames_dir = Path(output_dir) / 'frames'
    frames_dir.mkdir(parents=True, exist_ok=True)

    cam_path = frames_dir / f'{frame_num:04d}.cam.json'
    tmp_path = str(cam_path) + '.tmp'

    with open(tmp_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2)
    os.replace(tmp_path, str(cam_path))


def dlss_render_init(scene):
    """
    Called once at start of entire render job.
    Save user's resolution_percentage → override with DLSS quality value.
    """
    props = _get_props(scene)
    if not props or not props.enabled:
        return

    try:
        # Save user's original percentage (survives Blender crash via custom property)
        original_pct = scene.render.resolution_percentage
        scene['_dlss_saved_pct'] = original_pct

        # Override with DLSS quality-derived percentage
        dlss_pct = _get_quality_percentage(props.quality_mode)
        scene.render.resolution_percentage = dlss_pct

        log.info(f"DLSS engaged: original {original_pct}% → {dlss_pct}% (Quality={props.quality_mode})")
    except Exception as e:
        log.warning(f"DLSS render_init error (non-fatal): {e}")


def dlss_render_pre(scene, depsgraph=None):
    """
    Called before each frame renders.
    Collect camera + jitter data → write per-frame .cam.json
    """
    props = _get_props(scene)
    if not props or not props.enabled:
        return
    if '_dlss_saved_pct' not in scene:
        return  # render_init wasn't called properly

    try:
        frame_num = scene.frame_current
        output_dir = props.output_dir

        if not output_dir:
            log.warning("DLSS: output_dir not set — skipping per-frame cam.json")
            return

        cam_data = _collect_camera_data(scene, frame_num)
        _write_cam_json_atomic(output_dir, frame_num, cam_data)

    except Exception as e:
        log.warning(f"DLSS render_pre error (non-fatal): {e}")


def _finalize(scene):
    """
    Shared finalize logic for render_complete and render_cancel.
    Merge per-frame jsons → write dlssconfig.json → restore percentage.
    """
    props = _get_props(scene)
    if '_dlss_saved_pct' not in scene:
        return  # Nothing to restore

    try:
        saved_pct = scene['_dlss_saved_pct']

        # Write dlssconfig.json (merge per-frame cam.jsons)
        if props and props.output_dir:
            from .dlssconfig_writer import build_top_level_config, merge_per_frame_jsons, write_dlssconfig
            top_level = build_top_level_config(scene, props)
            frames = merge_per_frame_jsons(props.output_dir)
            try:
                write_dlssconfig(props.output_dir, top_level, frames)
                log.info(f"DLSS finalized: dlssconfig.json written ({len(frames)} frames)")
            except ValueError as e:
                log.warning(f"DLSS: dlssconfig.json schema validation failed: {e}")

        # Restore user's original percentage
        scene.render.resolution_percentage = int(saved_pct)
        del scene['_dlss_saved_pct']
        log.info(f"DLSS: restored resolution_percentage to {saved_pct}%")

    except Exception as e:
        log.warning(f"DLSS finalize error (non-fatal): {e}")


def dlss_render_complete(scene):
    """Called when render job completes successfully."""
    _finalize(scene)


def dlss_render_cancel(scene):
    """Called when render job is cancelled/interrupted."""
    _finalize(scene)


def dlss_load_post(scene=None):
    """
    Called when Blender starts or loads a .blend file.
    Crash recovery: if _dlss_saved_pct exists, restore it.
    """
    # load_post passes different args in different Blender versions
    # Try to get scene from various sources
    target_scene = scene
    if target_scene is None:
        try:
            target_scene = bpy.context.scene
        except AttributeError:
            return

    if '_dlss_saved_pct' in target_scene:
        saved_pct = target_scene['_dlss_saved_pct']
        current_pct = target_scene.render.resolution_percentage
        log.warning(
            f"DLSS detected stale saved_pct ({saved_pct}%, current {current_pct}%); restoring"
        )
        target_scene.render.resolution_percentage = int(saved_pct)
        del target_scene['_dlss_saved_pct']


def register_handlers():
    """Register all DLSS render handlers with Blender."""
    bpy.app.handlers.render_init.append(dlss_render_init)
    bpy.app.handlers.render_pre.append(dlss_render_pre)
    bpy.app.handlers.render_complete.append(dlss_render_complete)
    bpy.app.handlers.render_cancel.append(dlss_render_cancel)
    bpy.app.handlers.load_post.append(dlss_load_post)


def unregister_handlers():
    """Unregister all DLSS render handlers."""
    for handler, func in [
        (bpy.app.handlers.render_init, dlss_render_init),
        (bpy.app.handlers.render_pre, dlss_render_pre),
        (bpy.app.handlers.render_complete, dlss_render_complete),
        (bpy.app.handlers.render_cancel, dlss_render_cancel),
        (bpy.app.handlers.load_post, dlss_load_post),
    ]:
        if func in handler:
            handler.remove(func)
