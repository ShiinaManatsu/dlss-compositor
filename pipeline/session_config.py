"""Session-only pipeline policy shared by inject.py and the Electron app.

No bpy import — importable outside Blender.
"""
from __future__ import annotations

import json
import math
import re
from pathlib import Path

# Half-float multilayer EXR. ZIP usually lands near 5 MB/1080p scene color.
EXR_COLOR_DEPTH = "16"
EXR_COLOR_DEPTH_COMBINED = "32"
EXR_HALF_BYTES = 2
SR_EXR_CHANNELS = 4 + 1 + 4  # RGBA + Z + Vector
BEAUTY_EXR_CHANNELS = 4  # Combined only
GUIDES_EXR_CHANNELS = 1 + 4  # Z + Vector
RR_EXR_EXTRA_CHANNELS = 4 + 4 + 4 + 1 + 1 + 4 + 4 + 4  # RR File Output extras
DEFAULT_RAM_FILL_PERCENT = 90
DEFAULT_RAM_RESERVE_BYTES = 1 * 1024**3  # 留 1 GB 给运行 buffer
EST_ZIP_RATIO = 0.35
COLOR_SUBDIR = "color"
GUIDES_SUBDIR = "guides"

# DLSS SR §3.7.1.1. Ultra Quality 1.3x is the de-facto scale, not in the
# post-2.3.2 table, but NGX still exposes the enum.
DLSS_RATIO: dict[str, float] = {
    "DLAA": 1.0,
    "ULTRA_QUALITY": 1.3,
    "QUALITY": 1.5,
    "BALANCED": 1.724,
    "PERFORMANCE": 2.0,
    "ULTRA_PERFORMANCE": 3.0,
}
QUALITY_SLOTS = tuple(DLSS_RATIO.keys())

NR_RESERVED_MESSAGE = "DLSS5-NR 模型未到，接口已预留"
RR_FG_CREATE_FAILED = "RR+FG 创建失败"
IMDISK_MISSING_MESSAGE = "请安装 ImDisk Toolkit"
DRIVE_UNMOUNTED_RE = re.compile(r"drive\s+([A-Za-z]):\s+is not mounted", re.IGNORECASE)


def _json_error_text(raw: str) -> str | None:
    """Pull `message`/`error` out of a JSON event line. Ignore non-JSON."""
    decoder = json.JSONDecoder()
    for candidate in (raw, *raw.splitlines()):
        text = candidate.strip()
        if not text.startswith("{"):
            continue
        try:
            obj, _end = decoder.raw_decode(text)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            msg = obj.get("message") or obj.get("error")
            if msg:
                return str(msg)
    return None


def user_error_message(raw: object, *, letter: str = "R") -> str:
    """Product footer/status copy: short Chinese, never JSON, never EN+ZH.

    Inject this for failure-state tests. Do not unmount the RAM disk.
    """
    text = "" if raw is None else str(raw).strip()
    if not text:
        return "出错了"
    extracted = _json_error_text(text)
    if extracted:
        text = extracted.strip()
    drive = DRIVE_UNMOUNTED_RE.search(text)
    if drive:
        return f"{drive.group(1).upper()}: 未挂载"
    lowered = text.lower()
    if "will not create a ram disk" in lowered or "is not mounted" in lowered:
        letter_ch = (letter or "R").strip().rstrip(":").upper() or "R"
        return f"{letter_ch}: 未挂载"
    if "未找到卷标" in text:
        match = re.search(r"未找到卷标\s+(\S+)", text)
        label = (match.group(1) if match else "DLSSRAM").rstrip("的。")
        return f"未找到 {label}"
    if "export-dir must be on the working disk" in lowered:
        return "导出目录必须在工作盘，不能写到 RAM 盘"
    if text.lstrip().startswith("{") and '"event"' in text:
        return "出错了"
    return " ".join(text.split())


DEFAULT_FILENAME_TEMPLATE = "{job}_{mode}_v{version}"
_KNOWN_EXTS = {".png", ".exr", ".mov", ".mp4"}
_FRAME_TOKEN = re.compile(r"\{frame(?::0?\d+d)?\}")
_PLACEHOLDER = re.compile(r"\{([^{}]+)\}")


def parse_fg_multiplier(value: str | int | None) -> int:
    """Empty / omitted → 0 (FG off). Accepts 2, '2', '2x'..'6x'."""
    if value is None or value == "":
        return 0
    if isinstance(value, bool):
        raise ValueError(f"FG 倍率必须是 2x..6x，收到 {value}")
    if isinstance(value, int):
        raw = value
    else:
        text = str(value).strip().lower()
        if text in ("0", "off", "none"):
            return 0
        if text.endswith("x"):
            text = text[:-1]
        if not text:
            raise ValueError(f"FG 倍率必须是 2x..6x，收到 {value}")
        try:
            raw = int(text)
        except ValueError as exc:
            raise ValueError(f"FG 倍率必须是 2x..6x，收到 {value}") from exc
    if raw == 0:
        return 0
    if raw < 2 or raw > 6:
        raise ValueError(f"FG 倍率必须是 2x..6x，收到 {value}")
    return raw


def recommended_percentage(quality: str) -> int:
    ratio = DLSS_RATIO.get(quality.upper(), DLSS_RATIO["BALANCED"])
    return int(round(100.0 / ratio))


def resolve_render_percentage(
    percentage: int | None,
    quality: str,
    *,
    legacy_scale: bool = False,
    native_res_buffers: bool = False,
    mode: str = "sr",
) -> tuple[int | None, str]:
    """SR default leaves the blend percentage (quality-ratio path).

    RR only supports NGX quality slots (not arbitrary percentages). The
    render scale is always ``recommended_percentage(quality)`` unless
    ``native_res_buffers`` forces one output-res pass at 100%.
    ``legacy_scale`` without an explicit percentage uses the quality table
    for SR.
    """
    key = (mode or "sr").strip().lower()
    if native_res_buffers and key == "rr":
        return 100, "rr_native_output"
    if key == "rr":
        return recommended_percentage(quality), "rr_quality_scale"
    if percentage is not None:
        return int(percentage), "explicit"
    if legacy_scale:
        return recommended_percentage(quality), "legacy_quality_scale"
    return None, "blend"


def wants_dual_pass(
    *,
    native_res_buffers: bool,
    mode: str,
    percentage: int | None,
) -> bool:
    """SR + native_res_buffers + percentage < 100 → Beauty then Guides."""
    if not native_res_buffers:
        return False
    if (mode or "sr").strip().lower() != "sr":
        return False
    if percentage is None:
        return False
    return int(percentage) < 100


def estimate_exr_bytes(
    width: int,
    height: int,
    *,
    mode: str = "sr",
    half: bool = True,
    channels: int | None = None,
) -> int:
    """Conservative ZIP size for one multilayer EXR."""
    w = max(1, int(width))
    h = max(1, int(height))
    bpp = EXR_HALF_BYTES if half else 4
    if channels is None:
        nch = SR_EXR_CHANNELS
        if (mode or "sr").strip().lower() == "rr":
            nch += RR_EXR_EXTRA_CHANNELS
    else:
        nch = max(1, int(channels))
    uncompressed = w * h * bpp * nch
    zipped = int(uncompressed * EST_ZIP_RATIO)
    return max(1, zipped)


def estimate_window_frame_bytes(
    render_w: int,
    render_h: int,
    output_w: int,
    output_h: int,
    *,
    mode: str = "sr",
    dual_pass: bool = False,
) -> int:
    """Bytes one timeline frame occupies on the RAM disk."""
    if dual_pass:
        return estimate_exr_bytes(render_w, render_h, channels=BEAUTY_EXR_CHANNELS, half=True) + estimate_exr_bytes(
            output_w, output_h, channels=GUIDES_EXR_CHANNELS, half=True
        )
    return estimate_exr_bytes(render_w, render_h, mode=mode, half=True)


def pad_frame(n: int) -> str:
    return f"{int(n):04d}"


def phase_label(phase: str, start: int, end: int) -> str:
    names = {"beauty": "渲染", "guides": "引导", "rebuild": "重建", "combined": "渲染"}
    title = names.get(phase, phase)
    return f"{title} {pad_frame(start)}–{pad_frame(end)}"


def ram_window_budget(
    total_bytes: int,
    *,
    fill_percent: int = DEFAULT_RAM_FILL_PERCENT,
    reserve_bytes: int = DEFAULT_RAM_RESERVE_BYTES,
) -> int:
    """Max bytes the pipeline may occupy on the RAM disk.

    Preset is the tighter of: fill to ``fill_percent`` of capacity, or leave
    ``reserve_bytes`` free. Tiny disks that cannot honor the reserve still
    use the fill-percent cap so a single frame can proceed.
    """
    total = max(0, int(total_bytes))
    if total <= 0:
        return 0
    pct = min(100, max(1, int(fill_percent)))
    by_fill = total * pct // 100
    by_reserve = total - max(0, int(reserve_bytes))
    if by_reserve <= 0:
        return max(1, by_fill)
    return max(1, min(by_fill, by_reserve))


def plan_ram_windows(
    frame_start: int,
    frame_end: int,
    bytes_per_frame: int,
    budget_bytes: int,
    *,
    max_frames: int | None = None,
) -> list[tuple[int, int]]:
    """Split ``frame_start..frame_end`` into RAM-disk write windows.

    Each window's estimated EXR bytes stay ≤ budget. A single frame that
    exceeds the budget still gets its own window so the job cannot stall.
    ``max_frames`` caps frames per window (tests / forced two-window runs).
    """
    start = int(frame_start)
    end = int(frame_end)
    if end < start:
        return []
    per = max(1, int(bytes_per_frame))
    budget = max(1, int(budget_bytes))
    fit = max(1, budget // per)
    if max_frames is not None:
        fit = max(1, min(fit, int(max_frames)))
    windows: list[tuple[int, int]] = []
    cursor = start
    while cursor <= end:
        last = min(end, cursor + fit - 1)
        windows.append((cursor, last))
        cursor = last + 1
    return windows


def quality_mode_for_buffer(
    quality: str,
    render_w: int,
    render_h: int,
    output_w: int,
    output_h: int,
    *,
    mode: str = "sr",
) -> str:
    """Same-res full-buffer SR is DLAA for NGX. RR keeps the requested quality."""
    if (mode or "sr").strip().lower() != "rr" and render_w == output_w and render_h == output_h:
        return "DLAA"
    return quality.upper()


def ngx_min_render_size(output_w: int, output_h: int, quality: str) -> tuple[int, int]:
    """NGX GetRenderResolution floor for this output / quality.

    1080p Balanced SR min is 960×540 (DC-18 / DLSS SR §3.7). That is half of
    1920×1080. Ultra Performance allows 1/3; other modes allow 1/2.
    25% of 1080p (480×270) is below the floor and must be rejected.
    NGX reports a CEILed floor for odd sizes (1215→608, measured 2026-08-31),
    so ceil here — floor() would let a 1-px-too-small render through.
    """
    scale = 3.0 if quality.upper() == "ULTRA_PERFORMANCE" else 2.0
    return max(1, math.ceil(output_w / scale)), max(1, math.ceil(output_h / scale))


def validate_render_percentage(
    output_w: int,
    output_h: int,
    percentage: int,
    quality: str,
    *,
    mode: str = "sr",
) -> str | None:
    if percentage < 1 or percentage > 100:
        return f"渲染百分比 {percentage}% 无效"
    if (mode or "sr").strip().lower() == "rr":
        if quality.upper() == "ULTRA_QUALITY":
            return "RR 不支持超高质量档：请选 DLAA / 高质量 / 平衡 / 性能"
        if quality.upper() == "ULTRA_PERFORMANCE":
            third_w, third_h = output_w // 3, output_h // 3
            if third_w * 3 != output_w or third_h * 3 != output_h:
                return (
                    f"RR 超高性能要求渲染分辨率恰好为输出的 1/3，"
                    f"而 {output_w}x{output_h} 无法整除 3"
                )
    render_w = max(1, int(output_w * percentage / 100.0))
    render_h = max(1, int(output_h * percentage / 100.0))
    min_w, min_h = ngx_min_render_size(output_w, output_h, quality)
    if render_w < min_w or render_h < min_h:
        rec = recommended_percentage(quality)
        return (
            f"渲染分辨率 {render_w}×{render_h} 低于 NGX {quality} 下限 {min_w}×{min_h}"
            f"（1080p Balanced 为 960×540）。请提高到约 {rec}%，不要用 25%。"
        )
    return None


def is_sequence_format(fmt: str) -> bool:
    value = (fmt or "").lower()
    return value in {"png", "png_16", "exr", "exr_linear"}


def extension_for_format(fmt: str) -> str:
    value = (fmt or "").lower()
    if value in {"png", "png_16"}:
        return ".png"
    if value in {"exr", "exr_linear"}:
        return ".exr"
    if value in {"h265", "hevc", "h265_hevc"}:
        return ".mp4"
    return ".mov"


def job_token_from_blend(blend: str | None) -> str:
    if not blend:
        return ""
    name = Path(str(blend).replace("\\", "/")).name
    if name.lower().endswith(".blend"):
        name = name[: -len(".blend")]
    return _sanitize_filename_token(name)


def resolve_filename_template(
    template: str,
    *,
    job: str,
    mode: str,
    version: int,
    format: str,
) -> str:
    """Resolve `{job}` `{mode}` `{version}` `{frame}` or fail with a short error.

    Does not silently fall back to `output.mov`.
    """
    raw = (template or "").strip()
    if not raw:
        raise ValueError("文件名模板为空")
    job_token = _sanitize_filename_token(job)
    if "{job}" in raw and not job_token:
        raise ValueError("文件名模板无法解析: {job}")
    job_token = job_token or "job"
    mode_token = _sanitize_filename_token((mode or "sr").lower()) or "sr"
    ver = max(1, int(version) if version else 1)
    out = raw.replace("{job}", job_token).replace("{mode}", mode_token).replace("{version}", str(ver))
    out = out.replace("{frame}", "{frame:04d}")

    leftover: list[str] = []
    for match in _PLACEHOLDER.finditer(out):
        name = match.group(1)
        if not re.fullmatch(r"frame(?::0?\d+d)?", name):
            leftover.append(match.group(0))
    if leftover:
        raise ValueError("文件名模板无法解析: " + " ".join(leftover))

    sequence = is_sequence_format(format)
    if not sequence:
        out = re.sub(r"#+", "", out)
        out = _FRAME_TOKEN.sub("", out)
        out = _collapse_underscores(out)
    elif _FRAME_TOKEN.search(out) is None and "#" not in out:
        out = _append_before_extension(out, "_{frame:04d}")

    out = _collapse_underscores(out)
    stripped_frames = _FRAME_TOKEN.sub("", out)
    if "{" in stripped_frames or "}" in stripped_frames:
        raise ValueError("文件名模板无法解析")
    stem = _strip_known_extension(out)
    if not stem:
        raise ValueError("文件名模板无法解析")
    return _apply_format_extension(out, extension_for_format(format))


def _sanitize_filename_token(value: str) -> str:
    cleaned = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", value or "")
    return cleaned.rstrip(". ").strip()


def _collapse_underscores(value: str) -> str:
    value = re.sub(r"_+", "_", value)
    value = value.lstrip("_")
    value = re.sub(r"_+$", "", value)
    return re.sub(r"_+(?=\.[^.]+$)", "", value)


def _strip_known_extension(value: str) -> str:
    dot = value.rfind(".")
    if dot >= 0 and value[dot:].lower() in _KNOWN_EXTS:
        return value[:dot]
    return value


def _append_before_extension(value: str, extra: str) -> str:
    dot = value.rfind(".")
    if dot >= 0 and value[dot:].lower() in _KNOWN_EXTS:
        return f"{value[:dot]}{extra}{value[dot:]}"
    return f"{value}{extra}"


def _apply_format_extension(name: str, ext: str) -> str:
    dot = name.rfind(".")
    if dot >= 0 and name[dot:].lower() in _KNOWN_EXTS:
        return name[:dot] + ext
    return name + ext


def cycles_policy_for_mode(mode: str) -> dict[str, object]:
    key = (mode or "sr").strip().lower()
    if key == "nr" or key == "dlss5-nr" or key == "dlss5_nr":
        raise ValueError(NR_RESERVED_MESSAGE)
    if key == "rr":
        return {
            "use_denoising": False,
            "force_denoising": True,
            "denoising_store_passes": True,
            "require_cycles": True,
        }
    return {
        "use_denoising": None,
        "force_denoising": False,
        "denoising_store_passes": False,
        "require_cycles": False,
    }
