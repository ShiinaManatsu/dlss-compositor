"""OCIO LUT generation for the DLSS compositor Blender extension."""
from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from bpy.types import Scene

log = logging.getLogger("dlss_compositor.lut_generator")

DEFAULT_CUBE_SIZE = 65
DEFAULT_DOMAIN_MIN = 0.0
DEFAULT_DOMAIN_MAX = 1.0
DEFAULT_LOG2_MIN = -12.0
DEFAULT_LOG2_MAX = 4.0


@dataclass(frozen=True)
class GeneratedLuts:
    tonemap_lut: str | None = None
    inverse_lut: str | None = None


def _import_ocio() -> Any | None:
    try:
        import PyOpenColorIO as ocio  # type: ignore[import-not-found]
        return ocio
    except Exception:
        try:
            import OpenColorIO as ocio  # type: ignore[import-not-found]
            return ocio
        except Exception as exc:
            log.warning("OpenColorIO Python module unavailable; LUT generation skipped: %s", exc)
            return None


def _current_config(ocio: Any) -> Any:
    if hasattr(ocio, "GetCurrentConfig"):
        return ocio.GetCurrentConfig()
    return ocio.Config.CreateFromEnv()


def _scene_linear_space(ocio: Any, config: Any) -> str:
    role_names = []
    if hasattr(ocio, "ROLE_SCENE_LINEAR"):
        role_names.append(ocio.ROLE_SCENE_LINEAR)
    role_names.extend(["scene_linear", "rendering", "default"])

    for role in role_names:
        try:
            color_space = config.getRoleColorSpace(role)
            if color_space:
                return str(color_space)
        except Exception:
            pass

    candidates = [
        "Linear Rec.709",
        "Linear Rec.2020",
        "ACEScg",
        "Utility - Linear - sRGB",
        "scene_linear",
    ]
    for name in candidates:
        try:
            if config.getColorSpace(name) is not None:
                return name
        except Exception:
            pass

    raise RuntimeError("Could not find an OCIO scene-linear input color space")


def _processor_for_display_view(
    ocio: Any,
    config: Any,
    input_space: str,
    display: str,
    view: str,
    look: str,
    direction: Any,
) -> Any:
    transform_type = getattr(ocio, "DisplayViewTransform", None)
    if transform_type is None:
        if look:
            log.warning("OCIO DisplayViewTransform is unavailable; baking display/view without look=%s", look)
        return config.getProcessor(input_space, display, view, direction)

    transform = transform_type()
    for method_name, value in (
        ("setSrc", input_space),
        ("setDisplay", display),
        ("setView", view),
    ):
        method = getattr(transform, method_name, None)
        if method is not None:
            method(value)
    looks_method = getattr(transform, "setLooksOverride", None)
    if looks_method is None:
        looks_method = getattr(transform, "setLooks", None)
    if look and looks_method is not None:
        try:
            looks_method(look)
        except Exception as exc:
            log.warning(
                "OCIO DisplayViewTransform cannot set look=%s; baking display/view without look override: %s",
                look,
                exc,
            )
            look = ""
    elif look:
        log.warning("OCIO DisplayViewTransform cannot set look=%s; baking display/view without look override", look)
        look = ""
    looks_enabled_method = getattr(transform, "setLooksOverrideEnabled", None)
    if looks_enabled_method is not None:
        try:
            looks_enabled_method(bool(look))
        except Exception as exc:
            if look:
                log.warning(
                    "OCIO DisplayViewTransform cannot enable look override=%s; baking display/view without look override: %s",
                    look,
                    exc,
                )
    direction_method = getattr(transform, "setDirection", None)
    if direction_method is not None:
        direction_method(direction)

    try:
        return config.getProcessor(transform)
    except Exception as exc:
        if look:
            log.warning(
                "OCIO DisplayViewTransform processor failed with look=%s; baking display/view without look override: %s",
                look,
                exc,
            )
        return config.getProcessor(input_space, display, view, direction)


def _safe_name(value: str) -> str:
    safe = []
    for ch in value:
        if ch.isalnum() or ch in ("-", "_"):
            safe.append(ch)
        else:
            safe.append("_")
    return "".join(safe).strip("_") or "unknown"


def _build_grid(size: int, domain_min: float, domain_max: float) -> list[float]:
    values: list[float] = []
    span = domain_max - domain_min
    denom = float(size - 1)
    # .cube 3D LUT convention: red changes fastest, then green, then blue.
    for b in range(size):
        bv = domain_min + span * (float(b) / denom)
        for g in range(size):
            gv = domain_min + span * (float(g) / denom)
            for r in range(size):
                rv = domain_min + span * (float(r) / denom)
                values.extend((rv, gv, bv))
    return values


def _build_log2_scene_grid(size: int, log2_min: float, log2_max: float) -> list[float]:
    values: list[float] = []
    span = log2_max - log2_min
    denom = float(size - 1)

    def axis_value(index: int) -> float:
        if index <= 0:
            return 0.0
        t = float(index) / denom
        return 2.0 ** (log2_min + span * t)

    # .cube 3D LUT convention: red changes fastest, then green, then blue.
    for b in range(size):
        bv = axis_value(b)
        for g in range(size):
            gv = axis_value(g)
            for r in range(size):
                rv = axis_value(r)
                values.extend((rv, gv, bv))
    return values


def _apply_exposure(grid: list[float], exposure: float) -> list[float]:
    if exposure == 0.0:
        return grid
    scale = 2.0 ** exposure
    return [value * scale for value in grid]


def _apply_gamma(rgb: list[float], gamma: float) -> list[float]:
    if gamma == 1.0:
        return rgb
    exponent = 1.0 / gamma
    return [max(value, 0.0) ** exponent for value in rgb]


def _apply_inverse_gamma(rgb: list[float], gamma: float) -> list[float]:
    if gamma == 1.0:
        return rgb
    return [max(value, 0.0) ** gamma for value in rgb]


def _apply_inverse_exposure(grid: list[float], exposure: float) -> list[float]:
    if exposure == 0.0:
        return grid
    scale = 2.0 ** exposure
    return [value / scale for value in grid]


def _apply_rgb_grid(cpu: Any, grid: list[float]) -> list[float]:
    """Apply an OCIO CPUProcessor to a packed RGB grid.

    PyOpenColorIO bindings have differed here: some mutate packed RGB arrays
    in place, some return a new value, and some only accept one RGB triplet.
    """
    packed = list(grid)
    try:
        result = cpu.applyRGB(packed)
        if result is not None:
            converted = list(result)
            if len(converted) == len(grid):
                return converted
        if packed != grid:
            return packed
    except Exception:
        pass

    converted: list[float] = []
    for i in range(0, len(grid), 3):
        rgb = [grid[i + 0], grid[i + 1], grid[i + 2]]
        result = cpu.applyRGB(rgb)
        if result is not None:
            rgb = list(result)
        converted.extend((float(rgb[0]), float(rgb[1]), float(rgb[2])))
    return converted


def _write_cube(
    path: Path,
    title: str,
    size: int,
    domain_min: float,
    domain_max: float,
    rgb: list[float],
    metadata: dict[str, str] | None = None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f'TITLE "{title}"\n')
        if metadata:
            for key, value in metadata.items():
                f.write(f"# {key} {value}\n")
        f.write(f"LUT_3D_SIZE {size}\n")
        f.write(f"DOMAIN_MIN {domain_min:.9g} {domain_min:.9g} {domain_min:.9g}\n")
        f.write(f"DOMAIN_MAX {domain_max:.9g} {domain_max:.9g} {domain_max:.9g}\n")
        for i in range(0, len(rgb), 3):
            f.write(f"{rgb[i + 0]:.9g} {rgb[i + 1]:.9g} {rgb[i + 2]:.9g}\n")
    tmp.replace(path)


def generate_tonemap_lut(
    scene: Scene,
    output_dir: str,
    cube_size: int = DEFAULT_CUBE_SIZE,
    domain_min: float = DEFAULT_DOMAIN_MIN,
    domain_max: float = DEFAULT_DOMAIN_MAX,
) -> GeneratedLuts:
    """Bake the scene-linear -> active Blender display/view transform LUT.

    Returns relative paths suitable for dlssconfig.json. If OCIO is unavailable
    or Blender's current config cannot be baked, returns an empty result.
    """
    if not output_dir:
        log.warning("generate_tonemap_lut: output_dir is empty; LUT generation skipped")
        return GeneratedLuts()

    ocio = _import_ocio()
    if ocio is None:
        return GeneratedLuts()

    display = getattr(scene.display_settings, "display_device", "sRGB")
    view = getattr(scene.view_settings, "view_transform", "Filmic")
    look = getattr(scene.view_settings, "look", "")
    look_for_processor = "" if look == "None" else look
    exposure = float(getattr(scene.view_settings, "exposure", 0.0))
    gamma = float(getattr(scene.view_settings, "gamma", 1.0))
    out_root = Path(output_dir)
    name_suffix = (
        f"tonemap_{_safe_name(display)}_{_safe_name(view)}_{_safe_name(look_for_processor)}"
        f"_e{exposure:g}_g{gamma:g}_{cube_size}_{domain_max:g}.cube"
    )
    rel_path = Path("luts") / name_suffix
    lut_path = out_root / rel_path
    inverse_rel_path = Path("luts") / name_suffix.replace("tonemap_", "inverse_", 1).replace(
        f"_{domain_max:g}.cube",
        "_display.cube",
    )
    inverse_lut_path = out_root / inverse_rel_path

    try:
        config = _current_config(ocio)
        input_space = _scene_linear_space(ocio, config)
        forward = getattr(ocio, "TRANSFORM_DIR_FORWARD")
        inverse = getattr(ocio, "TRANSFORM_DIR_INVERSE")
        processor = _processor_for_display_view(
            ocio, config, input_space, display, view, look_for_processor, forward)
        cpu = processor.getDefaultCPUProcessor()

        grid = _build_log2_scene_grid(cube_size, DEFAULT_LOG2_MIN, DEFAULT_LOG2_MAX)
        grid = _apply_exposure(grid, exposure)
        baked = _apply_rgb_grid(cpu, grid)
        baked = _apply_gamma(baked, gamma)
        _write_cube(
            lut_path,
            f"{input_space} to {display}/{view} look={look_for_processor} exposure={exposure:g} gamma={gamma:g}",
            cube_size,
            domain_min,
            domain_max,
            baked,
            {
                "DLSS_LUT_INPUT_ENCODING": "log2",
                "DLSS_LUT_LOG2_MIN": f"{DEFAULT_LOG2_MIN:g}",
                "DLSS_LUT_LOG2_MAX": f"{DEFAULT_LOG2_MAX:g}",
            },
        )

        inverse_processor = _processor_for_display_view(
            ocio, config, input_space, display, view, look_for_processor, inverse)
        inverse_cpu = inverse_processor.getDefaultCPUProcessor()
        inverse_grid = _build_grid(cube_size, 0.0, 1.0)
        inverse_grid = _apply_inverse_gamma(inverse_grid, gamma)
        inverse_baked = _apply_rgb_grid(inverse_cpu, inverse_grid)
        inverse_baked = _apply_inverse_exposure(inverse_baked, exposure)
        _write_cube(
            inverse_lut_path,
            f"{display}/{view} look={look_for_processor} exposure={exposure:g} gamma={gamma:g} to {input_space}",
            cube_size,
            0.0,
            1.0,
            inverse_baked,
        )
        log.info(
            "Generated tonemap LUT %s and inverse LUT %s display=%s view=%s look=%s exposure=%.3g gamma=%.3g input=%s size=%d domain=[%.3g, %.3g]",
            lut_path,
            inverse_lut_path,
            display,
            view,
            look_for_processor,
            exposure,
            gamma,
            input_space,
            cube_size,
            domain_min,
            domain_max,
        )
        return GeneratedLuts(
            tonemap_lut=rel_path.as_posix(),
            inverse_lut=inverse_rel_path.as_posix(),
        )
    except Exception as exc:
        log.warning("generate_tonemap_lut failed; falling back to built-in shader: %s", exc)
        return GeneratedLuts()
