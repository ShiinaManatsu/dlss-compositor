"""One-off AOV EXR write benchmark: one multilayer node vs one node per pass.

Renders the same N frames through the pipeline's exact combined-buffer
wiring (RR pass set, Raw view, full-float, save_output off) and only
switches the writer topology:

  multilayer  the production node: 10 items in one OPEN_EXR_MULTILAYER
              node -> one .exr per frame
  single      one File Output node per pass (the UI "per-file" layout)
              -> 10 .exr per frame

Per-frame render_pre -> render_post spans include the compositor save, so
the mode difference is the write cost. --codec selects the EXR codec
(ZIP / NONE / ...). Never saves the blend; user File Output nodes are
muted for this session only.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pipeline"))

import bpy  # noqa: E402

from inject import (  # noqa: E402
    OUTPUT_LABEL,
    RR_FILE_ITEMS,
    compositor_tree,
    emit,
    enable_passes,
    find_render_layers,
    install_save_guard,
    insert_file_output,
)

# Combined-buffer layout in production wiring order: (R_Layers output,
# socket type). RR_FILE_ITEMS is (socket_type, name) tuples.
SINGLE_PASS_LAYOUT = (
    [("Image", "RGBA"), ("Depth", "FLOAT"), ("Vector", "VECTOR")]
    + [(name, sock) for sock, name in RR_FILE_ITEMS]
)


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--output-dir", required=True)
    p.add_argument("--mode", choices=("multilayer", "single", "none"), required=True)
    # Blender exr_codec identifier: ZIP / NONE / PIZ / RLE / ...
    p.add_argument("--codec", default="ZIP")
    p.add_argument("--frames", type=int, default=10)
    p.add_argument("--frame-start", type=int)
    p.add_argument("--file-name", default="bench")
    p.add_argument("--persistent", action="store_true",
                   help="render.use_persistent_data=True：帧间保留渲染数据，跳过逐帧场景同步/BVH 重建")
    p.add_argument("--cull", action="store_true",
                   help="cycles.use_camera_cull / use_distance_cull = True")
    p.add_argument("--tile", type=int,
                   help="cycles.tile_size（默认 2048）")
    return p.parse_args(argv)


def main() -> int:
    args = parse_args()
    install_save_guard()

    scene = bpy.context.scene
    render = scene.render
    render.engine = "CYCLES"
    render.use_sequencer = False
    render.use_overwrite = True
    # 关掉场景默认输出（save_output=False 只跳过 scene.render.filepath 的
    # 写出，合成器 File Output 节点照常写——inject.py 同款做法）。
    render.save_output = False

    enable_passes(scene, "rr", "TABULATED_SOBOL")
    scene.cycles.use_denoising = False  # RR policy: raw noisy frames to NGX.
    persistent_before = bool(render.use_persistent_data)
    if args.persistent:
        render.use_persistent_data = True
    if args.cull:
        for attr in ("use_camera_cull", "use_distance_cull"):
            if hasattr(scene.cycles, attr):
                setattr(scene.cycles, attr, True)
    if args.tile:
        try:
            scene.cycles.tile_size = args.tile
        except TypeError:
            pass

    tree = compositor_tree(scene)
    muted = [
        node.name
        for node in tree.nodes
        if node.type == "OUTPUT_FILE" and node.label != OUTPUT_LABEL
    ]
    for name in muted:
        tree.nodes[name].mute = True  # 本实验只写注入节点；仅本次会话静音。
    rl = find_render_layers(tree)
    out_dir = Path(args.output_dir)

    single = args.mode == "single"
    if args.mode == "none":
        # 基线：不挂任何写出口，量纯渲染时间。
        fo, linked = None, []
    elif single:
        # 每 pass 一个节点；生产节点先建再删，避免它跟着写一遍。
        fo, _ = insert_file_output(
            tree, out_dir, args.file_name, mode="rr", buffer_pass="combined"
        )
        tree.nodes.remove(fo)
        linked = []
        for out_name, sock_type in SINGLE_PASS_LAYOUT:
            slug = out_name.replace(" ", "")
            n = tree.nodes.new("CompositorNodeOutputFile")
            n.label = f"{OUTPUT_LABEL}_single"
            n.directory = str(out_dir.resolve()) + "\\"
            n.file_name = slug
            fmt = n.format
            fmt.file_format = "OPEN_EXR_MULTILAYER"  # 节点级仅此枚举；单层内容=单 pass 文件
            fmt.color_depth = "32"
            fmt.color_management = "OVERRIDE"
            fmt.view_settings.view_transform = "Raw"
            try:
                fmt.exr_codec = args.codec
            except TypeError:
                pass
            n.file_output_items.clear()
            item = n.file_output_items.new(sock_type, slug)
            if sock_type == "VECTOR" and hasattr(item, "vector_socket_dimensions"):
                item.vector_socket_dimensions = 4
            sock = rl.outputs.get(out_name)
            if sock is None:
                emit("error", message=f"R_Layers 缺输出 {out_name!r}")
                return 1
            tree.links.new(sock, n.inputs[0])
            linked.append(out_name)
    else:
        fo, linked = insert_file_output(
            tree, out_dir, args.file_name, mode="rr", buffer_pass="combined"
        )
        fo.format.color_depth = "32"
        try:
            fo.format.exr_codec = args.codec
        except TypeError:
            pass

    frame_start = (
        int(args.frame_start) if args.frame_start is not None else int(scene.frame_start)
    )
    scene.frame_start = frame_start
    scene.frame_end = frame_start + args.frames - 1

    span = {"t": time.perf_counter(), "change": time.perf_counter()}
    t_all = time.perf_counter()

    def on_change(*_):
        span["change"] = time.perf_counter()

    def on_pre(*_):
        span["t"] = time.perf_counter()
        # frame_change_post（depsgraph 完）→ render_pre 的间隙 = 渲染循环
        # 进入 Cycles 前的剩余工作。
        span["init"] = span["t"] - span["change"]

    def on_post(scn, *_):
        emit(
            "frame_done",
            mode=args.mode,
            frame=int(scn.frame_current),
            span_seconds=round(time.perf_counter() - span["t"], 3),
            pre_gap_seconds=round(span.get("init", 0.0), 3),
        )

    bpy.app.handlers.frame_change_post.append(on_change)
    bpy.app.handlers.render_pre.append(on_pre)
    bpy.app.handlers.render_post.append(on_post)

    emit(
        "bench_start",
        mode=args.mode,
        codec=args.codec,
        frames=args.frames,
        frame_start=frame_start,
        frame_end=int(scene.frame_end),
        res=f"{render.resolution_x}x{render.resolution_y}@{render.resolution_percentage}",
        samples=int(scene.cycles.samples),
        persistent_before=persistent_before,
        persistent=bool(render.use_persistent_data),
        muted=muted,
        linked=linked,
    )

    if args.mode == "none":
        # Blender 拒绝「save_output 关 + 无 File Output」的动画渲染（Error:
        # Render output disabled ... no active File Output nodes），基线改用
        # 逐帧 render(write_still=False)，不落任何盘。
        for f in range(frame_start, frame_start + args.frames):
            scene.frame_set(f)
            bpy.ops.render.render(write_still=False)
    else:
        bpy.ops.render.render(animation=True)

    files = sorted(out_dir.rglob("*.exr")) if out_dir.is_dir() else []
    total_bytes = sum(f.stat().st_size for f in files)
    emit(
        "bench_summary",
        mode=args.mode,
        wall_seconds=round(time.perf_counter() - t_all, 2),
        files=len(files),
        bytes=total_bytes,
        sample=[f.name for f in files[:4]],
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        emit("error", message=str(exc))
        raise
