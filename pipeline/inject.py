"""Session-only Blender inject for pipeline-v3.

Invoked as:

    blender -b <blend> --python inject.py -- --output-dir <ramdisk> ...

Never saves the blend (no wm.save / save_as / save_homefile).
Inserts a compositor File Output; does not rewrite the user's existing nodes.
Vector socket is VECTOR with 4 dimensions (RGBA-width), matching DC-16.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import time
from pathlib import Path
from typing import Any

# Blender puts the script directory on sys.path; keep an explicit insert for
# `blender --python pipeline/inject.py` from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import bpy  # noqa: E402

from jitter import (  # noqa: E402
    JITTER_PHASES,
    apply_jitter_to_frame,
    jitter_for_frame_number,
)
from inspect_scene import (  # noqa: E402
    apply_scene_overrides,
    collect_cycles_compute,
    existing_file_outputs,
    parse_bool,
    refresh_cycles_devices,
)
from session_config import (  # noqa: E402
    EXR_COLOR_DEPTH,
    EXR_COLOR_DEPTH_COMBINED,
    NR_RESERVED_MESSAGE,
    cycles_policy_for_mode,
    quality_mode_for_buffer,
    validate_render_percentage,
)

_PLUGIN_LUT = Path(__file__).resolve().parent.parent / "blender-plugin" / "dlss_compositor"
if str(_PLUGIN_LUT) not in sys.path:
    sys.path.insert(0, str(_PLUGIN_LUT))
from lut_generator import generate_tonemap_lut  # noqa: E402

OUTPUT_LABEL = "DLSS_Pipeline_V3"
SAVE_OPERATORS = (
    "wm.save_mainfile",
    "wm.save_as_mainfile",
    "wm.save_homefile",
)
QUALITY_SCHEMA = {
    "DLAA": "dlaa",
    "ULTRA_QUALITY": "ultra_quality",
    "QUALITY": "quality",
    "BALANCED": "balanced",
    "PERFORMANCE": "performance",
    "ULTRA_PERFORMANCE": "ultra_performance",
}
HDR_DISPLAY_DEVICES = {"Rec.2100-PQ", "Rec.2100-HLG"}

# RR guide buffers required by DLSS-RR (nvsdk_ngx_helpers_dlssd.h).
# All VECTOR items need vector_socket_dimensions = 4 (RGBA-width EXR channel layout).
# Blender 5.3 needs the separate denoising_store_motion_passes flag for the last two.
# "Denoising Backward Motion"  → packed previous-frame motion used internally by RR
# "Denoising Specular Motion"  → pInMotionVectorsReflection (reflection motion vectors)
RR_FILE_ITEMS = (
    ("RGBA",   "Denoising Albedo"),           # pInDiffuseAlbedo
    ("RGBA",   "Denoising Specular Albedo"),   # pInSpecularAlbedo
    ("VECTOR", "Denoising Normal"),            # pInNormals
    ("FLOAT",  "Denoising Roughness"),         # pInRoughness
    ("FLOAT",  "Denoising Depth"),             # pInDepth (linear)
    ("VECTOR", "Denoising Backward Motion"),   # backward optical flow (Blender 5.3+)
    ("VECTOR", "Denoising Specular Motion"),   # pInMotionVectorsReflection (Blender 5.3+)
)

# Blender 5.3 Cycles sampling_pattern identifiers.
# AUTOMATIC defers to Blender's default (currently blue-noise in 5.3).
# TABULATED_SOBOL is white-noise-like; recommended by DLSS-RR §3.5 for best
# temporal stability. BLUE_NOISE is the 5.3 default — conditionally acceptable.
SAMPLING_PATTERN_AUTO = "AUTOMATIC"
SAMPLING_PATTERN_WHITE = "TABULATED_SOBOL"
SAMPLING_PATTERN_BLUE = "BLUE_NOISE"


def emit(event: str, **fields: object) -> None:
    sys.stdout.write("DLSS_EVENT:" + json.dumps({"event": event, **fields}, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def argv_after_double_dash() -> list[str]:
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="pipeline-v3 session inject (never saves)")
    parser.add_argument("--output-dir", required=True, help="RAM-disk output directory")
    parser.add_argument(
        "--config-dir",
        default=None,
        help="directory for dlssconfig.json; default is --output-dir",
    )
    parser.add_argument("--percentage", type=int, default=None, help="render resolution_percentage")
    parser.add_argument("--frame-start", type=int, default=None)
    parser.add_argument("--frame-end", type=int, default=None)
    parser.add_argument(
        "--config-frame-start",
        type=int,
        default=None,
        help="dlssconfig camera range start; default is --frame-start",
    )
    parser.add_argument(
        "--config-frame-end",
        type=int,
        default=None,
        help="dlssconfig camera range end; default is --frame-end",
    )
    parser.add_argument(
        "--skip-config",
        action="store_true",
        help="insert File Output and render, but do not rewrite dlssconfig.json",
    )
    parser.add_argument(
        "--quality",
        default="BALANCED",
        choices=sorted(JITTER_PHASES.keys()),
        help="DLSS quality slot (jitter table; RR render scale is chosen by run.py from this)",
    )
    parser.add_argument("--file-name", default="####")
    parser.add_argument("--worker-publish-dir", type=Path, default=None,
                        help="accept frame windows from stdin; publish complete EXRs here")
    parser.add_argument("--worker-free-floor", type=int, default=0,
                        help="minimum free bytes before a worker starts a frame")
    parser.add_argument(
        "--pass",
        dest="buffer_pass",
        default="combined",
        choices=("combined", "beauty", "guides"),
        help="combined: color+Z+Vector. beauty: half-float Combined only. guides: Vector+Z at output res.",
    )
    parser.add_argument("--dry-run", action="store_true", help="insert nodes, do not render")
    parser.add_argument("--engine", default=None, help="optional render engine override")
    parser.add_argument("--samples", type=int, default=None, help="Cycles max samples; omit to leave blend value")
    parser.add_argument(
        "--use-adaptive-sampling",
        type=parse_bool,
        default=None,
        help="Cycles adaptive sampling; omit to leave blend value",
    )
    parser.add_argument("--adaptive-threshold", type=float, default=None, help="Noise Threshold")
    parser.add_argument("--adaptive-min-samples", type=int, default=None, help="Cycles min samples")
    parser.add_argument("--time-limit", type=float, default=None, help="Cycles time limit seconds")
    parser.add_argument(
        "--use-denoising",
        type=parse_bool,
        default=None,
        help="Cycles denoise; ignored in RR (always off)",
    )
    parser.add_argument("--denoiser", default=None, help="OPENIMAGEDENOISE | OPTIX")
    parser.add_argument(
        "--perf-persistent",
        type=parse_bool,
        default=None,
        help="render.use_persistent_data; default true (perf fallback)",
    )
    parser.add_argument(
        "--perf-camera-cull",
        type=parse_bool,
        default=None,
        help="cycles.use_camera_cull; default true (perf fallback)",
    )
    parser.add_argument(
        "--camera-cull-margin",
        type=float,
        default=None,
        help="cycles.camera_cull_margin; omit to keep blend value",
    )
    parser.add_argument(
        "--perf-distance-cull",
        type=parse_bool,
        default=None,
        help="cycles.use_distance_cull; default true (perf fallback)",
    )
    parser.add_argument(
        "--distance-cull-margin",
        type=float,
        default=None,
        help="cycles.distance_cull_margin; omit to keep blend value",
    )
    parser.add_argument(
        "--tile-size",
        type=int,
        default=None,
        help="cycles.tile_size; default 4096 (perf fallback)",
    )
    parser.add_argument(
        "--sampling-pattern",
        dest="sampling_pattern",
        default=None,
        choices=("AUTOMATIC", "TABULATED_SOBOL", "BLUE_NOISE"),
        help=(
            "Cycles sampling pattern. TABULATED_SOBOL = white-noise-like (recommended by RR §3.5). "
            "BLUE_NOISE = Blender 5.3 default. AUTOMATIC = defer to blend. "
            "Only applied in RR mode; ignored for SR."
        ),
    )
    parser.add_argument(
        "--mode",
        default="sr",
        choices=("sr", "rr", "nr"),
        help="sr: Combined/Z/Vector. rr: Cycles denoising-data File Output items. nr: reserved.",
    )
    parser.add_argument("--output-width", type=int, default=None, help="export/output resolution_x")
    parser.add_argument("--output-height", type=int, default=None, help="export/output resolution_y")
    parser.add_argument("--fps", type=int, default=None, help="scene fps; default keep blend or 30")
    parser.add_argument(
        "--view-transform",
        default=None,
        help="ignored: colour follows the blend (kept so old CLI flags do not crash)",
    )
    parser.add_argument("--display-device", default=None, help="ignored: colour follows the blend")
    parser.add_argument("--look", default=None, help="ignored: colour follows the blend")
    parser.add_argument("--exposure", type=float, default=None, help="ignored: colour follows the blend")
    parser.add_argument("--gamma", type=float, default=None, help="ignored: colour follows the blend")
    parser.add_argument(
        "--preview-dir",
        default=None,
        help="If set, keep scene output on and write JPEG previews here",
    )
    parser.add_argument(
        "--preview-jpeg-quality",
        type=int,
        default=5,
        help="JPEG quality 1-100 for --preview-dir (default 5, tiny files)",
    )
    return parser.parse_args(argv_after_double_dash())


def refuse_save(*_args: Any, **_kwargs: Any) -> None:
    raise RuntimeError("pipeline-v3 inject refuses to save the blend")


def install_save_guard() -> None:
    wm = bpy.ops.wm
    for name in SAVE_OPERATORS:
        try:
            setattr(wm, name, refuse_save)
        except Exception:
            pass


def compositor_tree(scene: bpy.types.Scene) -> bpy.types.NodeTree:
    scene.render.use_compositing = True
    effects = getattr(scene, "compositor_effects", None)
    if effects is not None:
        # The legacy Scene property resolves to the active effect. Inject into
        # a separate pass-through effect so existing groups and order stay intact.
        tree = bpy.data.node_groups.new(name=OUTPUT_LABEL, type="CompositorNodeTree")
        tree.allow_usage_in_scene_compositor_effect = True
        for direction in ("OUTPUT", "INPUT"):
            tree.interface.new_socket(name="Image", in_out=direction, socket_type="NodeSocketColor")
        source = tree.nodes.new("NodeGroupInput")
        sink = tree.nodes.new("NodeGroupOutput")
        tree.links.new(source.outputs["Image"], sink.inputs["Image"])
        effect = effects.new(OUTPUT_LABEL)
        effect.node_group = tree
        effect.enable_for_preview = False
        return tree
    tree = scene.compositing_node_group
    if tree is None:
        tree = bpy.data.node_groups.new(name="Compositor", type="CompositorNodeTree")
        scene.compositing_node_group = tree
    return tree


def find_render_layers(tree: bpy.types.NodeTree) -> bpy.types.Node:
    for node in tree.nodes:
        if node.type == "R_LAYERS":
            return node
    node = tree.nodes.new("CompositorNodeRLayers")
    node.location = (-400, 0)
    return node


def _link_if_present(tree: bpy.types.NodeTree, rl: bpy.types.Node, fo: bpy.types.Node, out_name: str, inp_idx: int) -> bool:
    sock = rl.outputs.get(out_name)
    if sock is None or inp_idx >= len(fo.inputs):
        return False
    tree.links.new(sock, fo.inputs[inp_idx])
    return True


def insert_file_output(
    tree: bpy.types.NodeTree,
    output_dir: Path,
    file_name: str,
    mode: str = "sr",
    buffer_pass: str = "combined",
) -> tuple[bpy.types.Node, list[str]]:
    for node in list(tree.nodes):
        if node.label == OUTPUT_LABEL:
            tree.nodes.remove(node)

    fo = tree.nodes.new("CompositorNodeOutputFile")
    fo.label = OUTPUT_LABEL
    fo.location = (400, 0)
    # Blender treats paths without a "//" prefix as absolute; a relative
    # --output-dir would be an invalid drive-less path and writes fail silently.
    # ImDisk volumes can reject the Windows final-path query used by resolve().
    fo.directory = str(Path(output_dir).absolute()) + "\\"
    fo.file_name = file_name

    fmt = fo.format
    fmt.file_format = "OPEN_EXR_MULTILAYER"
    fmt.color_management = "OVERRIDE"
    fmt.view_settings.view_transform = "Raw"
    depth = EXR_COLOR_DEPTH if buffer_pass == "beauty" else EXR_COLOR_DEPTH_COMBINED
    if hasattr(fmt, "color_depth"):
        try:
            fmt.color_depth = depth
        except TypeError:
            pass
    if hasattr(fmt, "exr_codec"):
        try:
            fmt.exr_codec = "ZIP"
        except TypeError:
            pass

    fo.file_output_items.clear()
    extra_names: list[str] = []
    links: list[tuple[str, int]] = []
    if buffer_pass == "beauty":
        item_combined = fo.file_output_items.new("RGBA", "")
        item_combined.name = ""
        links = [("Image", 0)]
    elif buffer_pass == "guides":
        item_z = fo.file_output_items.new("FLOAT", "Z")
        item_z.name = "Z"
        item_vec = fo.file_output_items.new("VECTOR", "Vector")
        item_vec.name = "Vector"
        item_vec.vector_socket_dimensions = 4
        links = [("Depth", 0), ("Vector", 1)]
    else:
        item_combined = fo.file_output_items.new("RGBA", "")
        item_combined.name = ""
        item_z = fo.file_output_items.new("FLOAT", "Z")
        item_z.name = "Z"
        item_vec = fo.file_output_items.new("VECTOR", "Vector")
        item_vec.name = "Vector"
        item_vec.vector_socket_dimensions = 4
        links = [("Image", 0), ("Depth", 1), ("Vector", 2)]
        if mode == "rr":
            for socket_type, name in RR_FILE_ITEMS:
                item = fo.file_output_items.new(socket_type, name)
                item.name = name
                if socket_type == "VECTOR" and hasattr(item, "vector_socket_dimensions"):
                    item.vector_socket_dimensions = 4
                extra_names.append(name)

    rl = find_render_layers(tree)
    linked = []
    for out_name, inp_idx in links:
        if _link_if_present(tree, rl, fo, out_name, inp_idx):
            linked.append(out_name)

    if mode == "rr" and buffer_pass == "combined":
        for name in extra_names:
            inp = fo.inputs.get(name)
            sock = rl.outputs.get(name)
            if inp is None or sock is None:
                continue
            tree.links.new(sock, inp)
            linked.append(name)

    return fo, linked


def apply_perf_fallbacks(scene: bpy.types.Scene, args: argparse.Namespace) -> None:
    """Session-only render speed fallbacks (never saved into the blend).

    The per-frame "Initializing" phase dominates animation exports on heavy
    scenes; these shrink the sync/BVH/upload work Cycles redoes each frame.
    Switches default on; margins follow the blend unless overridden; tile
    defaults to 4096. All guarded — older Blenders just skip missing attrs.
    """
    render = scene.render
    cycles = getattr(scene, "cycles", None)
    # Keep render data resident between frames: skips per-frame scene sync and
    # BVH rebuild for everything static.
    if hasattr(render, "use_persistent_data"):
        render.use_persistent_data = True if args.perf_persistent is None else args.perf_persistent
    if cycles is not None:
        # Cull objects outside the camera frustum / beyond the distance margin
        # before sync: less geometry to sync and BVH per frame.
        for attr, value in (
            ("use_camera_cull", args.perf_camera_cull),
            ("use_distance_cull", args.perf_distance_cull),
        ):
            if hasattr(cycles, attr):
                setattr(cycles, attr, True if value is None else value)
        if args.camera_cull_margin is not None and hasattr(cycles, "camera_cull_margin"):
            cycles.camera_cull_margin = args.camera_cull_margin
        if args.distance_cull_margin is not None and hasattr(cycles, "distance_cull_margin"):
            cycles.distance_cull_margin = args.distance_cull_margin
        # 4K frames exceed the 2048 default: one tile avoids per-tile overhead
        # (denoiser/filter setup per tile) on GPU renders.
        tile = 4096 if args.tile_size is None else args.tile_size
        if hasattr(cycles, "tile_size"):
            try:
                cycles.tile_size = tile
            except TypeError:
                pass


def enable_passes(scene: bpy.types.Scene, mode: str = "sr", sampling_pattern: str | None = None) -> None:
    for view_layer in scene.view_layers:
        view_layer.use_pass_z = True
        view_layer.use_pass_vector = True
        if mode == "rr":
            cycles_vl = getattr(view_layer, "cycles", None)
            if cycles_vl is not None:
                if hasattr(cycles_vl, "denoising_store_passes"):
                    cycles_vl.denoising_store_passes = True
                if hasattr(cycles_vl, "denoising_store_motion_passes"):
                    cycles_vl.denoising_store_motion_passes = True
                if hasattr(cycles_vl, "denoising_pass_follow_reflections"):
                    # Follow reflected surfaces when constructing the RR guides.
                    cycles_vl.denoising_pass_follow_reflections = True
    policy = cycles_policy_for_mode(mode)
    if hasattr(scene, "cycles") and policy["denoising_store_passes"] and hasattr(scene.cycles, "denoising_store_passes"):
        scene.cycles.denoising_store_passes = True

    # Apply sampling pattern override (RR mode only; ignored for SR).
    # TABULATED_SOBOL is white-noise-like and recommended by DLSS-RR §3.5.
    if mode == "rr" and sampling_pattern and hasattr(scene, "cycles"):
        try:
            scene.cycles.sampling_pattern = sampling_pattern
        except (TypeError, AttributeError):
            pass  # Older Blender — best-effort, no abort.

    # Distinct noise/dither pattern per frame: with a fixed seed every frame
    # carries the same noise, and temporal DLSS passes (RR accumulation, FG
    # synthesis) blend it into a persistent dirt pattern. Animated seed ties
    # the Cycles seed to the frame number.
    if hasattr(scene, "cycles") and hasattr(scene.cycles, "use_animated_seed"):
        try:
            scene.cycles.use_animated_seed = True
        except (TypeError, AttributeError):
            pass  # Older Blender — best-effort, no abort.

    if hasattr(scene.render, "use_lock_interface"):
        scene.render.use_lock_interface = True


def vector_item_report(fo: bpy.types.Node) -> dict[str, object]:
    for item in fo.file_output_items:
        if item.name == "Vector":
            return {
                "name": item.name,
                "socket_type": getattr(item, "socket_type", None),
                "vector_socket_dimensions": getattr(item, "vector_socket_dimensions", None),
            }
    return {}


def collect_camera_data(
    scene: bpy.types.Scene,
    jitter_x: float,
    jitter_y: float,
    depsgraph: Any = None,
) -> dict[str, Any]:
    camera = scene.camera
    matrix_world = [[0.0] * 4 for _ in range(4)]
    projection = [[0.0] * 4 for _ in range(4)]
    fov = 45.0
    aspect_ratio = 1.0
    near_clip = 0.1
    far_clip = 100.0

    if camera is not None:
        for i in range(4):
            for j in range(4):
                matrix_world[i][j] = float(camera.matrix_world[i][j])
        if hasattr(camera.data, "angle"):
            fov = float(camera.data.angle)
        if hasattr(camera.data, "clip_start"):
            near_clip = float(camera.data.clip_start)
        if hasattr(camera.data, "clip_end"):
            far_clip = float(camera.data.clip_end)

        render = scene.render
        if render.resolution_y > 0:
            aspect_ratio = render.resolution_x / render.resolution_y
        render_w = max(1, int(render.resolution_x * render.resolution_percentage / 100.0))
        render_h = max(1, int(render.resolution_y * render.resolution_percentage / 100.0))

        if depsgraph is not None and hasattr(camera, "calc_matrix_camera"):
            proj = camera.calc_matrix_camera(
                depsgraph,
                x=render_w,
                y=render_h,
                scale_x=render.pixel_aspect_x,
                scale_y=render.pixel_aspect_y,
            )
            for i in range(4):
                for j in range(4):
                    projection[i][j] = float(proj[i][j])
        else:
            angle_x = float(getattr(camera.data, "angle_x", 0.0) or 0.0)
            angle_y = float(getattr(camera.data, "angle_y", 0.0) or 0.0)
            if angle_x <= 0.0 or angle_y <= 0.0:
                if fov > 0.0 and aspect_ratio > 0.0:
                    angle_y = fov
                    angle_x = 2.0 * math.atan(math.tan(angle_y * 0.5) * aspect_ratio)
                else:
                    angle_y = math.radians(45.0)
                    angle_x = angle_y
            tan_half_x = math.tan(angle_x * 0.5)
            tan_half_y = math.tan(angle_y * 0.5)
            proj_x = 1.0 / tan_half_x if tan_half_x > 0.0 else 1.0
            proj_y = 1.0 / tan_half_y if tan_half_y > 0.0 else 1.0
            denom = near_clip - far_clip
            proj_z = (far_clip + near_clip) / denom if denom != 0.0 else -1.0
            proj_w = (2.0 * far_clip * near_clip) / denom if denom != 0.0 else -(2.0 * near_clip)
            shift_x = float(getattr(camera.data, "shift_x", 0.0))
            shift_y = float(getattr(camera.data, "shift_y", 0.0))
            projection[0][0] = proj_x
            projection[0][2] = 2.0 * shift_x
            projection[1][1] = proj_y
            projection[1][2] = 2.0 * shift_y
            projection[2][2] = proj_z
            projection[2][3] = proj_w
            projection[3][2] = -1.0

    return {
        "matrix_world": matrix_world,
        "projection": projection,
        "fov": fov,
        "aspect_ratio": aspect_ratio,
        "near_clip": near_clip,
        "far_clip": far_clip,
        "jitter_x": jitter_x,
        "jitter_y": jitter_y,
    }


def write_atomic_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with tmp_path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, ensure_ascii=False)
    os.replace(tmp_path, path)


# ── Camera depsgraph isolation ────────────────────────────────────────────────
# The bake only reads the camera's transform/projection per frame, but
# scene.frame_set() re-evaluates the whole scene (modifiers, sims, rigs).
# Hiding every object outside the camera's dependency closure drops them from
# the viewport depsgraph, making frame_set cost only the camera chain.


def _driver_target_ids(animation_data: Any) -> list[Any]:
    """IDs referenced by driver variables on this datablock."""
    refs: list[Any] = []
    if animation_data is None:
        return refs
    for fcurve in animation_data.drivers:
        try:
            for var in fcurve.driver.variables:
                for target in var.targets:
                    if target.id is not None:
                        refs.append(target.id)
        except Exception:
            continue
    return refs


def _driver_reaches_bpy(animation_data: Any) -> bool:
    """Python driver expressions can reach bpy.data directly — references the
    static analysis below cannot see, so their presence disables isolation."""
    if animation_data is None:
        return False
    for fcurve in animation_data.drivers:
        try:
            expression = fcurve.driver.expression or ""
        except Exception:
            continue
        if "bpy" in expression:
            return True
    return False


def _constraint_target_objects(constraint: Any) -> list[Any]:
    """Every Object pointer on a constraint (target, shrinkwrap mesh, …)."""
    refs: list[Any] = []
    for prop in constraint.bl_rna.properties:
        if prop.type != "POINTER":
            continue
        try:
            value = getattr(constraint, prop.identifier)
        except Exception:
            continue
        if isinstance(value, bpy.types.Object):
            refs.append(value)
    return refs


# Sims on the camera chain may read hidden objects (collision/emit partners) —
# their per-frame result would silently change, so isolation bails instead.
_SIM_MODIFIER_TYPES = {
    "CLOTH",
    "SOFT_BODY",
    "FLUID",
    "FLUID_SIMULATION",
    "PARTICLE_SYSTEM",
    "DYNAMIC_PAINT",
}


def camera_dependency_closure(scene: bpy.types.Scene) -> set | None:
    """Objects that can affect the camera's per-frame transform/projection:
    the camera, its parent chain, constraint targets, DOF focus object and
    every Object referenced by drivers on the closure's objects or data
    datablocks. None = closure not provably complete (python drivers reaching
    bpy.data, or a simulation on the chain)."""
    camera = scene.camera
    if camera is None:
        return None
    unsafe = False
    seen: set = {camera}
    queue = [camera]
    while queue:
        obj = queue.pop()
        datas = [obj.animation_data]
        if obj.data is not None:
            datas.append(getattr(obj.data, "animation_data", None))
        for anim in datas:
            if anim is None:
                continue
            if _driver_reaches_bpy(anim):
                unsafe = True
            for ref in _driver_target_ids(anim):
                if isinstance(ref, bpy.types.Object) and ref not in seen:
                    seen.add(ref)
                    queue.append(ref)
        for constraint in obj.constraints:
            for ref in _constraint_target_objects(constraint):
                if ref not in seen:
                    seen.add(ref)
                    queue.append(ref)
        if obj.type == "CAMERA" and obj.data is not None:
            focus = obj.data.dof.focus_object
            if focus is not None and focus not in seen:
                seen.add(focus)
                queue.append(focus)
        parent = obj.parent
        if parent is not None and parent not in seen:
            seen.add(parent)
            queue.append(parent)
        if obj.rigid_body is not None or obj.rigid_body_constraint is not None:
            unsafe = True
        for modifier in obj.modifiers:
            if modifier.type in _SIM_MODIFIER_TYPES:
                unsafe = True
    return None if unsafe else seen


def apply_camera_isolation(scene: bpy.types.Scene) -> list[tuple[Any, bool]]:
    """Hide every object outside the camera's dependency closure. Returns the
    (object, previous state) pairs to restore — empty when skipped."""
    closure = camera_dependency_closure(scene)
    if closure is None:
        emit("bake_isolation", applied=False, reason="unsafe-dependency")
        return []
    hidden: list[tuple[Any, bool]] = []
    for obj in scene.objects:
        if obj in closure or obj.hide_viewport:
            continue
        try:
            hidden.append((obj, bool(obj.hide_viewport)))
            obj.hide_viewport = True
        except Exception:
            continue  # linked/read-only objects stay evaluated — conservative
    emit("bake_isolation", applied=True, hidden=len(hidden), total=len(scene.objects))
    return hidden


def restore_camera_isolation(hidden: list[tuple[Any, bool]]) -> None:
    for obj, state in hidden:
        try:
            obj.hide_viewport = state
        except Exception:
            pass


def write_dlssconfig(
    output_dir: Path,
    scene: bpy.types.Scene,
    quality: str,
    frame_start: int,
    frame_end: int,
    mode: str,
) -> Path:
    render = scene.render
    output_w = int(render.resolution_x)
    output_h = int(render.resolution_y)
    pct = int(render.resolution_percentage)
    render_w = max(1, int(output_w * pct / 100.0))
    render_h = max(1, int(output_h * pct / 100.0))
    display_device = str(getattr(scene.display_settings, "display_device", "sRGB"))
    view_transform = str(getattr(scene.view_settings, "view_transform", "Raw"))
    look = str(getattr(scene.view_settings, "look", "None"))
    exposure = float(getattr(scene.view_settings, "exposure", 0.0))
    gamma = float(getattr(scene.view_settings, "gamma", 1.0))
    depsgraph = bpy.context.evaluated_depsgraph_get() if hasattr(bpy.context, "evaluated_depsgraph_get") else None

    frames: dict[str, Any] = {}
    saved_frame = scene.frame_current
    ngx_quality = quality_mode_for_buffer(
        quality, render_w, render_h, output_w, output_h, mode=mode
    )
    if ngx_quality not in JITTER_PHASES:
        ngx_quality = quality
    # Per-frame camera eval is the slow part of a long-range bake — stream
    # progress so the GUI can show it (orchestrator maps it into the first 10%).
    total_bake = frame_end - frame_start + 1
    emit_every = max(1, total_bake // 100)
    # Drop everything the camera cannot see from the depsgraph: with the rest
    # of the scene hidden, frame_set only evaluates the camera chain.
    hidden = apply_camera_isolation(scene)
    try:
        for i, frame_num in enumerate(range(frame_start, frame_end + 1)):
            scene.frame_set(frame_num)
            jitter_x, jitter_y = jitter_for_frame_number(ngx_quality, frame_num)
            frames[f"{frame_num:04d}"] = collect_camera_data(scene, jitter_x, jitter_y, depsgraph)
            if i % emit_every == 0 or i == total_bake - 1:
                emit("bake_progress", percent=(i + 1) / total_bake, frame=frame_num)
        scene.frame_set(saved_frame)
    finally:
        restore_camera_isolation(hidden)

    luts = generate_tonemap_lut(scene, str(output_dir))
    config = {
        "version": 1,
        "output_resolution": {"width": output_w, "height": output_h},
        "render_resolution": {"width": render_w, "height": render_h},
        "quality_mode": QUALITY_SCHEMA.get(ngx_quality, "dlaa"),
        "display_device": display_device,
        "view_transform": view_transform,
        "look": look,
        "exposure": exposure,
        "gamma": gamma,
        "is_hdr": display_device in HDR_DISPLAY_DEVICES,
        "fps": int(getattr(render, "fps", 30) or 30),
        "filename_pattern": "####.exr",
        "jitter_phases": JITTER_PHASES[ngx_quality if ngx_quality in JITTER_PHASES else quality],
        "frames": frames,
        "buffer": "fullres_half" if render_w == output_w and render_h == output_h else "legacy_scale",
        "exr_color_depth": EXR_COLOR_DEPTH,
    }
    if luts.tonemap_lut:
        config["tonemap_lut"] = luts.tonemap_lut
    if luts.inverse_lut:
        config["inverse_lut"] = luts.inverse_lut
    emit(
        "lut_baked",
        view_transform=view_transform,
        display_device=display_device,
        look=look,
        tonemap_lut=luts.tonemap_lut or "",
        inverse_lut=luts.inverse_lut or "",
    )
    path = output_dir / "dlssconfig.json"
    write_atomic_json(path, config)
    return path


def main() -> int:
    install_save_guard()
    args = parse_args()
    output_dir = Path(args.output_dir)
    if not args.dry_run:
        output_dir.mkdir(parents=True, exist_ok=True)
        if args.config_dir:
            Path(args.config_dir).mkdir(parents=True, exist_ok=True)

    if args.mode == "nr":
        emit("error", message=NR_RESERVED_MESSAGE)
        return 3

    scene = bpy.context.scene
    render = scene.render
    refresh_cycles_devices(bpy.context.preferences)
    cycles_compute = collect_cycles_compute(scene, bpy.context.preferences)

    # App-side params (not blend override pins).
    if args.output_width is not None:
        render.resolution_x = args.output_width
    if args.output_height is not None:
        render.resolution_y = args.output_height
    if args.fps is not None:
        render.fps = args.fps
        if hasattr(render, "fps_base"):
            render.fps_base = 1.0
    elif int(getattr(render, "fps", 0) or 0) <= 0:
        render.fps = 30

    policy = cycles_policy_for_mode(args.mode)
    if policy["require_cycles"] and args.engine and args.engine != "CYCLES":
        emit("error", message=f"RR 需要 Cycles，收到引擎 {args.engine}")
        return 1
    if args.engine and not policy["require_cycles"]:
        render.engine = args.engine

    buffer_pass = str(getattr(args, "buffer_pass", "combined") or "combined")
    if buffer_pass == "guides":
        args.percentage = 100
        args.samples = 1
        args.use_adaptive_sampling = False
        args.use_denoising = False
        args.adaptive_min_samples = 1
        args.time_limit = 0.0
        emit(
            "guides_pass",
            message="引导趟：输出分辨率、1 spp、Vector+Z，Combined 不当 color",
            percentage=100,
            samples=1,
        )

    override_spec = {
        "resolution_percentage": args.percentage,
        "frame_start": args.frame_start,
        "frame_end": args.frame_end,
        "use_adaptive_sampling": args.use_adaptive_sampling,
        "adaptive_threshold": args.adaptive_threshold,
        "samples": args.samples,
        "adaptive_min_samples": args.adaptive_min_samples,
        "time_limit": args.time_limit,
        "use_denoising": args.use_denoising,
        "denoiser": args.denoiser,
    }
    applied = apply_scene_overrides(scene, override_spec, args.mode)
    if "engine" in applied["forced"]:
        emit("engine_forced", previous=applied["before"]["render"]["engine"], engine="CYCLES")

    percent_error = validate_render_percentage(
        int(render.resolution_x),
        int(render.resolution_y),
        int(render.resolution_percentage),
        args.quality,
        mode=args.mode,
    )
    if percent_error:
        emit("error", message=percent_error)
        return 1

    # Session-only: disable the scene-level animation output entirely. Blender
    # writes scene.render.filepath (e.g. preview_####.jpg) in addition to File
    # Output nodes; save_output=False skips that writer (and its encode) while
    # compositor File Output nodes keep writing — this is its designed use
    # (animation render just requires an active File Output node, which we
    # insert below). Optional --preview-dir keeps scene output on for tiny JPEGs.
    render.use_sequencer = False
    render.use_overwrite = True
    preview_dir = getattr(args, "preview_dir", None)
    if preview_dir:
        render.save_output = True
        prev = Path(preview_dir)
        prev.mkdir(parents=True, exist_ok=True)
        render.filepath = str(prev / "####")
        render.image_settings.file_format = "JPEG"
        render.image_settings.color_mode = "RGB"
        q = int(getattr(args, "preview_jpeg_quality", 5) or 5)
        render.image_settings.quality = max(1, min(100, q))
    else:
        render.save_output = False

    frame_start = int(scene.frame_start)
    frame_end = int(scene.frame_end)
    total = frame_end - frame_start + 1

    fullres = int(render.resolution_percentage) == 100
    emit(
        "inject_start",
        message="满分辨率 scene color（half float）写入内存盘" if fullres else "按指定百分比渲染后写入内存盘",
        blend=bpy.data.filepath,
        output_dir=str(output_dir),
        percentage=render.resolution_percentage,
        quality=args.quality,
        mode=args.mode,
        frame_start=frame_start,
        frame_end=frame_end,
        exr_color_depth=EXR_COLOR_DEPTH,
        skip_config=bool(args.skip_config),
        buffer_pass=buffer_pass,
        dry_run=args.dry_run,
        will_save_blend=False,
        overrides_written=applied["written"],
        overrides_untouched=applied["untouched"],
        overrides_forced=applied["forced"],
        overrides_ignored=applied["ignored"],
        cycles_compute=cycles_compute,
    )

    enable_passes(scene, args.mode, sampling_pattern=getattr(args, "sampling_pattern", None))
    apply_perf_fallbacks(scene, args)
    preexisting = existing_file_outputs(scene, exclude_label=OUTPUT_LABEL)
    tree = compositor_tree(scene)
    buffer_pass = str(getattr(args, "buffer_pass", "combined") or "combined")
    fo, linked_sockets = insert_file_output(
        tree, output_dir, args.file_name, args.mode, buffer_pass=buffer_pass
    )
    leftover = existing_file_outputs(scene, exclude_label=OUTPUT_LABEL)
    if leftover != preexisting:
        emit(
            "error",
            message="existing File Output nodes were mutated; aborting",
            before=preexisting,
            after=leftover,
        )
        return 1
    vec = vector_item_report(fo)
    if buffer_pass != "beauty" and vec.get("vector_socket_dimensions") != 4:
        emit("error", message="Vector socket is not 4-dimensional", vector=vec)
        return 1

    camera = scene.camera
    base_shift_x = float(camera.data.shift_x) if camera is not None else 0.0
    base_shift_y = float(camera.data.shift_y) if camera is not None else 0.0
    render_w = max(1, int(render.resolution_x * render.resolution_percentage / 100.0))
    render_h = max(1, int(render.resolution_y * render.resolution_percentage / 100.0))
    quality = quality_mode_for_buffer(
        args.quality, render_w, render_h, int(render.resolution_x), int(render.resolution_y), mode=args.mode
    )
    if quality not in JITTER_PHASES:
        quality = args.quality

    def wait_for_buffer_headroom() -> None:
        """Block until the RAM-disk buffer has headroom again.

        Blender can out-render the CLI consumer; once the fixed-size RAM disk fills,
        writes come out truncated and frames are lost. Gating each frame on free
        space makes the pipeline run at the consumer's pace instead of overflowing.
        """
        out_dir = Path(args.output_dir)
        try:
            total = shutil.disk_usage(out_dir).total
        except OSError:
            return  # Not an fs-backed path — nothing to gate on.
        if total <= 0:
            return
        worker = args.worker_publish_dir is not None
        floor = args.worker_free_floor if worker else max(1 << 30, int(total * 0.25))
        warned = False
        deadline = time.monotonic() + 30.0
        while True:
            try:
                free = shutil.disk_usage(out_dir).free
            except OSError:
                return
            if free >= floor:
                if warned:
                    emit("buffer_recovered", free=free)
                return
            if not warned:
                emit("buffer_full", free=free, floor=floor)
                warned = True
            if worker and time.monotonic() >= deadline:
                message = f"缓存可用空间不足：{free} 字节，至少需要 {floor} 字节"
                emit("error", message=message)
                raise RuntimeError(message)
            time.sleep(0.5)

    @bpy.app.handlers.persistent
    def on_render_pre(update_scene: bpy.types.Scene) -> None:
        wait_for_buffer_headroom()
        frame_num = update_scene.frame_current
        jitter_x, jitter_y = jitter_for_frame_number(quality, frame_num)
        apply_jitter_to_frame(update_scene, jitter_x, jitter_y, base_shift_x, base_shift_y)
        emit(
            "progress",
            frame=frame_num,
            total=total,
            jitter_x=jitter_x,
            jitter_y=jitter_y,
        )

    @bpy.app.handlers.persistent
    def on_render_post(update_scene: bpy.types.Scene) -> None:
        emit("frame_done", frame=update_scene.frame_current, total=total)

    bpy.app.handlers.render_pre.append(on_render_pre)
    if hasattr(bpy.app.handlers, "render_post"):
        bpy.app.handlers.render_post.append(on_render_post)

    config_start = args.config_frame_start if args.config_frame_start is not None else frame_start
    config_end = args.config_frame_end if args.config_frame_end is not None else frame_end
    config_dir = Path(args.config_dir) if args.config_dir else output_dir
    config_path = config_dir / "dlssconfig.json"
    if args.skip_config:
        emit(
            "config_skipped",
            message="沿用已有 dlssconfig，不重写",
            config=str(config_path),
            config_exists=config_path.is_file(),
        )
        if not config_path.is_file():
            emit("error", message="--skip-config 需要已有 dlssconfig.json")
            return 1
    else:
        config_path = write_dlssconfig(
            config_dir, scene, args.quality, config_start, config_end, args.mode
        )
    emit(
        "inject_ready",
        file_output=fo.name,
        directory=fo.directory,
        existing_file_outputs=preexisting,
        muted_file_outputs=[],
        vector=vec,
        linked_sockets=linked_sockets,
        mode=args.mode,
        config=str(config_path),
        view_transform=str(getattr(scene.view_settings, "view_transform", "") or ""),
        display_device=str(getattr(scene.display_settings, "display_device", "") or ""),
        look=str(getattr(scene.view_settings, "look", "") or ""),
        file_output_view=fo.format.view_settings.view_transform,
        use_denoising=bool((applied["after"].get("cycles") or {}).get("use_denoising")),
        denoising_store_passes=bool(
            (applied["after"].get("cycles") or {}).get("denoising_store_passes")
        ),
        engine=str(render.engine),
        fps=int(getattr(render, "fps", 30) or 30),
        is_dirty=bool(bpy.data.is_dirty),
        will_save_blend=False,
        overrides_written=applied["written"],
        overrides_untouched=applied["untouched"],
        overrides_forced=applied["forced"],
        overrides_ignored=applied["ignored"],
        cycles_compute=cycles_compute,
        scene_before=applied["before"],
        scene_after=applied["after"],
        denoiser=str(getattr(getattr(scene, "cycles", None), "denoiser", "") or ""),
        samples=int(getattr(getattr(scene, "cycles", None), "samples", 0) or 0),
        adaptive_threshold=float(
            getattr(getattr(scene, "cycles", None), "adaptive_threshold", 0.0) or 0.0
        ),
        use_adaptive_sampling=bool(
            getattr(getattr(scene, "cycles", None), "use_adaptive_sampling", False)
        ),
        adaptive_min_samples=int(
            getattr(getattr(scene, "cycles", None), "adaptive_min_samples", 0) or 0
        ),
        time_limit=float(getattr(getattr(scene, "cycles", None), "time_limit", 0.0) or 0.0),
        resolution_percentage=int(render.resolution_percentage),
        frame_start=frame_start,
        frame_end=frame_end,
        sampling_pattern=str(getattr(getattr(scene, "cycles", None), "sampling_pattern", "") or ""),
    )

    if args.dry_run:
        emit("dry_run_complete", is_dirty=bool(bpy.data.is_dirty), saved=False)
        return 0

    if args.worker_publish_dir is None:
        bpy.ops.render.render(animation=True)
    else:
        publish_dir = args.worker_publish_dir.absolute()
        if not publish_dir.is_dir():
            raise NotADirectoryError(str(publish_dir))
        written = 0
        publish_error = None

        @bpy.app.handlers.persistent
        def publish_frame(update_scene: bpy.types.Scene) -> None:
            nonlocal written, publish_error
            if publish_error is not None:
                return
            try:
                # File Output is done at render_write. The consumer only sees
                # the atomic rename, so it cannot decode a half-written file.
                files = list(output_dir.glob("*.exr"))
                frame = update_scene.frame_current
                if len(files) != 1 or int(files[0].stem) != frame or files[0].stat().st_size == 0:
                    raise RuntimeError(f"帧 {update_scene.frame_current} 未写出完整的 EXR")
                os.replace(files[0], publish_dir / f"{frame:04d}.exr")
                written += 1
                emit("worker_frame", frame=frame)
            except Exception as exc:
                publish_error = str(exc)
                emit("error", message=publish_error)

        bpy.app.handlers.render_write.append(publish_frame)
        scene.frame_step = 1
        emit("worker_ready")
        for line in sys.stdin:
            command = json.loads(line)
            first, last = command["start"], command["end"]
            if type(first) is not int or type(last) is not int or not frame_start <= first <= last <= frame_end:
                raise ValueError(f"无效的渲染窗口：{first}–{last}")
            scene.frame_end = last
            scene.frame_start = first
            written = 0
            status = bpy.ops.render.render(animation=True)
            if publish_error or "FINISHED" not in status or written != last - first + 1:
                raise RuntimeError(publish_error or f"窗口 {first}–{last} 只写出 {written} 帧")
            if camera is not None:
                camera.data.shift_x = base_shift_x
                camera.data.shift_y = base_shift_y
            emit("worker_done", start=first, end=last)

    if camera is not None:
        camera.data.shift_x = base_shift_x
        camera.data.shift_y = base_shift_y

    emit(
        "complete",
        is_dirty=bool(bpy.data.is_dirty),
        saved=False,
        output_dir=str(output_dir),
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        emit("error", message=str(exc))
        raise
