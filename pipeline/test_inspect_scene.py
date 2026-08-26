"""Unit checks for scene inspect + opt-in overrides. No Blender."""
from __future__ import annotations

from inspect_scene import (
    apply_ocio_config,
    apply_scene_overrides,
    collect_cycles_compute,
    collect_scene_state,
    inject_override_args,
    snapshot_ocio,
)
from session_config import cycles_policy_for_mode


class Obj:
    def __init__(self, **kwargs: object) -> None:
        self.__dict__.update(kwargs)


class EnumItem:
    def __init__(self, identifier: str) -> None:
        self.identifier = identifier


class EnumProp:
    def __init__(self, items: list[str]) -> None:
        self.enum_items = [EnumItem(item) for item in items]


def make_scene() -> Obj:
    return Obj(
        render=Obj(
            resolution_x=1920,
            resolution_y=1080,
            resolution_percentage=100,
            fps=24,
            fps_base=1.0,
            engine="CYCLES",
        ),
        cycles=Obj(
            use_adaptive_sampling=True,
            adaptive_threshold=0.01,
            samples=4096,
            adaptive_min_samples=0,
            time_limit=0.0,
            use_denoising=True,
            denoiser="OPENIMAGEDENOISE",
            preview_denoiser="OPTIX",
            denoising_store_passes=False,
            device="GPU",
        ),
        frame_start=1,
        frame_end=250,
        view_settings=Obj(
            view_transform="ACES 2.0",
            look="None",
            bl_rna=Obj(
                properties={
                    "view_transform": EnumProp(
                        ["ACES 2.0", "ACES 1.3", "AgX", "Standard", "False Color", "Raw"]
                    )
                }
            ),
        ),
        display_settings=Obj(
            display_device="sRGB",
            bl_rna=Obj(
                properties={
                    "display_device": EnumProp(["sRGB", "Rec.1886", "Rec.2020", "Rec.2100-PQ"])
                }
            ),
        ),
        compositing_node_group=Obj(
            nodes=[
                Obj(type="OUTPUT_FILE", name="File Output", label="User", mute=False, directory="C:\\out", file_name="####"),
                Obj(type="COMPOSITE", name="Composite", label="", mute=False, directory="", file_name=""),
            ]
        ),
    )


def test_collect_reports_cycles_and_file_outputs() -> None:
    scene = make_scene()
    state = collect_scene_state(scene, blend="asuka.blend")
    assert state["ok"] is True
    assert state["resolution_x"] == 1920
    assert state["resolution_percentage"] == 100
    assert state["frame_start"] == 1
    assert state["frame_end"] == 250
    assert state["fps"] == 24.0
    assert state["engine"] == "CYCLES"
    assert state["cycles"]["samples"] == 4096
    assert state["cycles"]["adaptive_threshold"] == 0.01
    assert state["cycles"]["denoiser"] == "OPENIMAGEDENOISE"
    assert state["cycles"]["denoiser_label"] == "OIDN"
    assert state["cycles"]["preview_denoiser"] == "OPTIX"
    assert state["cycles"]["preview_denoiser_label"] == "OptiX"
    assert state["cycles"]["device"] == "GPU"
    assert state["file_output_count"] == 1
    assert state["file_outputs"][0]["label"] == "User"
    assert state["view_transform"] == "ACES 2.0"
    assert state["look"] == "None"
    assert state["display_device"] == "sRGB"
    assert "Filmic" not in state["view_transforms"]
    assert state["view_transforms"] == [
        "ACES 2.0",
        "ACES 1.3",
        "AgX",
        "Standard",
        "False Color",
        "Raw",
    ]
    assert "sRGB" in state["display_devices"]


def test_ocio_does_not_invent_filmic() -> None:
    scene = make_scene()
    ocio = snapshot_ocio(scene)
    assert "Filmic" not in ocio["view_transforms"]
    assert "Khronos PBR Neutral" not in ocio["view_transforms"]
    assert ocio["view_transform"] == "ACES 2.0"


class FakeOcio:
    def getDisplays(self) -> list[str]:
        return ["sRGB", "Rec.2100-PQ"]

    def getViews(self, display: str) -> list[str]:
        if display == "sRGB":
            return ["Standard", "ACES 1.3", "ACES 2.0", "AgX", "False Color", "Raw"]
        return ["ACES 2.0 - HDR 1000 nits", "Raw"]


def test_apply_ocio_config_uses_current_display_views() -> None:
    scene = make_scene()
    payload = collect_scene_state(scene)
    payload = apply_ocio_config(payload, FakeOcio())
    assert payload["view_transform"] == "ACES 2.0"
    assert "ACES 2.0" in payload["view_transforms"]
    assert "Filmic" not in payload["view_transforms"]
    assert payload["views_by_display"]["sRGB"][2] == "ACES 2.0"
    assert "Rec.2100-PQ" in payload["display_devices"]


def test_collect_cycles_compute_from_prefs() -> None:
    scene = make_scene()
    prefs = Obj(
        addons={
            "cycles": Obj(
                preferences=Obj(
                    compute_device_type="OPTIX",
                    devices=[
                        Obj(name="NVIDIA GeForce RTX 5090", type="OPTIX", use=True),
                        Obj(name="Intel CPU", type="CPU", use=False),
                    ],
                )
            )
        }
    )
    info = collect_cycles_compute(scene, prefs)
    assert info["scene_device"] == "GPU"
    assert info["compute_device_type"] == "OPTIX"
    assert info["devices"][0]["use"] is True


def test_empty_overrides_write_nothing() -> None:
    scene = make_scene()
    result = apply_scene_overrides(scene, {}, "sr")
    assert result["written"] == []
    assert "samples" in result["untouched"]
    assert "resolution_percentage" in result["untouched"]
    assert "use_denoising" in result["untouched"]
    assert scene.cycles.samples == 4096
    assert scene.cycles.adaptive_threshold == 0.01
    assert scene.cycles.use_denoising is True
    assert scene.render.resolution_percentage == 100
    assert scene.frame_start == 1
    assert scene.frame_end == 250


def test_samples_override_leaves_threshold() -> None:
    scene = make_scene()
    result = apply_scene_overrides(scene, {"samples": 32}, "sr")
    assert result["written"] == ["samples"]
    assert "adaptive_threshold" in result["untouched"]
    assert scene.cycles.samples == 32
    assert scene.cycles.adaptive_threshold == 0.01
    assert scene.cycles.use_denoising is True


def test_rr_forces_denoise_off_and_ignores_user_pin() -> None:
    scene = make_scene()
    result = apply_scene_overrides(scene, {"use_denoising": True, "samples": 16}, "rr")
    assert "samples" in result["written"]
    assert "use_denoising" in result["forced"]
    assert "use_denoising" in result["ignored"]
    assert scene.cycles.use_denoising is False
    assert scene.cycles.denoising_store_passes is True
    assert scene.cycles.samples == 16
    assert scene.render.engine == "CYCLES"


def test_sr_does_not_force_denoise() -> None:
    policy = cycles_policy_for_mode("sr")
    assert policy["force_denoising"] is False
    scene = make_scene()
    scene.cycles.use_denoising = False
    apply_scene_overrides(scene, {}, "sr")
    assert scene.cycles.use_denoising is False


def test_cycles_missing_denoiser_is_hard_error() -> None:
    scene = make_scene()
    scene.cycles.denoiser = ""
    try:
        collect_scene_state(scene)
        raise AssertionError("missing denoiser must fail")
    except ValueError as exc:
        assert "denoiser" in str(exc)


def test_inject_args_omit_unchecked() -> None:
    assert inject_override_args({}) == []
    args = inject_override_args({"samples": 32, "frame_start": 100, "frame_end": 104})
    assert args == ["--frame-start", "100", "--frame-end", "104", "--samples", "32"]
    bool_args = inject_override_args({"use_adaptive_sampling": False, "use_denoising": True})
    assert bool_args == [
        "--use-adaptive-sampling",
        "false",
        "--use-denoising",
        "true",
    ]


if __name__ == "__main__":
    test_collect_reports_cycles_and_file_outputs()
    test_ocio_does_not_invent_filmic()
    test_apply_ocio_config_uses_current_display_views()
    test_collect_cycles_compute_from_prefs()
    test_empty_overrides_write_nothing()
    test_samples_override_leaves_threshold()
    test_rr_forces_denoise_off_and_ignores_user_pin()
    test_sr_does_not_force_denoise()
    test_cycles_missing_denoiser_is_hard_error()
    test_inject_args_omit_unchecked()
    print("test_inspect_scene ok")
