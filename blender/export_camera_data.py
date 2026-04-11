"""DLSS Compositor — Camera Data Exporter for Blender.

Exports per-frame camera matrices (world and projection), FOV, aspect ratio,
and clipping planes to a JSON file consumed by the DLSS Frame Generation
pipeline.

Run headless with:

    blender --background <file.blend> --python export_camera_data.py \
        -- --output camera.json [--start 1] [--end 250]

Use ``--test`` to validate argument parsing without a .blend file:

    blender --background --factory-startup --python export_camera_data.py -- --test
"""

import json
import math
import os
import sys


def _parse_args():
    """Parse arguments after the ``--`` separator."""
    argv = []
    if "--" in sys.argv:
        argv = sys.argv[sys.argv.index("--") + 1 :]

    parsed = {
        "output": None,
        "start": None,
        "end": None,
        "test": False,
    }

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--test":
            parsed["test"] = True
        elif arg == "--output" and i + 1 < len(argv):
            i += 1
            parsed["output"] = argv[i]
        elif arg == "--start" and i + 1 < len(argv):
            i += 1
            parsed["start"] = int(argv[i])
        elif arg == "--end" and i + 1 < len(argv):
            i += 1
            parsed["end"] = int(argv[i])
        i += 1

    return parsed


def _matrix_to_list(mat):
    """Convert a Blender Matrix to a row-major list of 4 lists of 4 floats."""
    return [list(row) for row in mat]


def _export_camera_data(output_path, frame_start, frame_end):
    """Walk the frame range and write camera data JSON."""
    import bpy

    scene = bpy.context.scene
    if scene is None:
        print("ERROR: No active scene.", file=sys.stderr)
        sys.exit(1)

    cam_obj = scene.camera

    if cam_obj is None:
        print("ERROR: No active camera in the scene.", file=sys.stderr)
        sys.exit(1)

    cam_data = getattr(cam_obj, "data", None)
    fov = getattr(cam_data, "angle", None)
    near_clip = getattr(cam_data, "clip_start", None)
    far_clip = getattr(cam_data, "clip_end", None)
    if fov is None or near_clip is None or far_clip is None:
        print("ERROR: Active camera is missing required camera data.", file=sys.stderr)
        sys.exit(1)

    if frame_end < frame_start:
        print(
            f"ERROR: frame range is empty (start={frame_start}, end={frame_end}).",
            file=sys.stderr,
        )
        sys.exit(1)

    render = scene.render
    if render is None:
        print("ERROR: Scene render settings are unavailable.", file=sys.stderr)
        sys.exit(1)

    render_width = render.resolution_x
    render_height = render.resolution_y

    data = {
        "version": 1,
        "render_width": render_width,
        "render_height": render_height,
        "frames": {},
    }

    depsgraph = bpy.context.evaluated_depsgraph_get()

    for frame_num in range(frame_start, frame_end + 1):
        scene.frame_set(frame_num)
        depsgraph.update()

        key = f"{frame_num:04d}"

        matrix_world = _matrix_to_list(cam_obj.matrix_world)
        projection = _matrix_to_list(
            cam_obj.calc_matrix_camera(
                depsgraph,
                x=render_width,
                y=render_height,
            )
        )

        data["frames"][key] = {
            "matrix_world": matrix_world,
            "projection": projection,
            "fov": fov,
            "aspect_ratio": render_width / render_height,
            "near_clip": near_clip,
            "far_clip": far_clip,
        }

    # Ensure output directory exists
    out_dir = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(out_dir, exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)

    print(f"Exported {len(data['frames'])} frame(s) to {output_path}")


# ---------------------------------------------------------------------------
# Entry point (headless / script mode)
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    try:
        args = _parse_args()

        if args["test"]:
            import bpy  # noqa: F811
            import tempfile

            tmp = tempfile.mktemp(suffix=".json")
            _export_camera_data(tmp, 1, 1)
            with open(tmp, "r", encoding="utf-8") as fh:
                result = json.load(fh)

            assert "frames" in result, "Missing 'frames' key"
            assert "0001" in result["frames"], "Missing frame 0001"

            frame = result["frames"]["0001"]
            assert (
                "matrix_world" in frame
                and len(frame["matrix_world"]) == 4
                and all(len(row) == 4 for row in frame["matrix_world"])
            )
            assert (
                "projection" in frame
                and len(frame["projection"]) == 4
                and all(len(row) == 4 for row in frame["projection"])
            )
            assert frame["fov"] > 0, "fov must be positive"
            assert frame["aspect_ratio"] > 0, "aspect_ratio must be positive"
            assert frame["near_clip"] > 0
            assert frame["far_clip"] > frame["near_clip"]

            os.unlink(tmp)

            print("OK")
            sys.exit(0)

        if args["output"] is None:
            print(
                "ERROR: --output is required.  Usage:\n"
                "  blender --background <file.blend> --python "
                "export_camera_data.py -- --output camera.json "
                "[--start 1] [--end 250]",
                file=sys.stderr,
            )
            sys.exit(1)

        import bpy  # noqa: F811

        scene = bpy.context.scene
        if scene is None:
            print("ERROR: No active scene.", file=sys.stderr)
            sys.exit(1)

        frame_start = args["start"] if args["start"] is not None else scene.frame_start
        frame_end = args["end"] if args["end"] is not None else scene.frame_end

        _export_camera_data(args["output"], frame_start, frame_end)

    except SystemExit:
        raise
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
