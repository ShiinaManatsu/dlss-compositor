"""Unit checks for RR policy and NGX render-percentage floor. No Blender."""
from __future__ import annotations

from session_config import (
    DEFAULT_FILENAME_TEMPLATE,
    DEFAULT_RAM_FILL_PERCENT,
    DEFAULT_RAM_RESERVE_BYTES,
    EXR_COLOR_DEPTH,
    NR_RESERVED_MESSAGE,
    RR_FG_CREATE_FAILED,
    cycles_policy_for_mode,
    estimate_exr_bytes,
    estimate_window_frame_bytes,
    job_token_from_blend,
    ngx_min_render_size,
    parse_fg_multiplier,
    phase_label,
    plan_ram_windows,
    quality_mode_for_buffer,
    ram_window_budget,
    recommended_percentage,
    resolve_filename_template,
    resolve_render_percentage,
    user_error_message,
    validate_render_percentage,
    wants_dual_pass,
)


def test_rr_turns_off_cycles_denoiser() -> None:
    policy = cycles_policy_for_mode("rr")
    assert policy["use_denoising"] is False
    assert policy["force_denoising"] is True
    assert policy["denoising_store_passes"] is True
    assert policy["require_cycles"] is True


def test_sr_does_not_force_cycles_denoiser() -> None:
    policy = cycles_policy_for_mode("sr")
    assert policy["force_denoising"] is False
    assert policy["use_denoising"] is None
    assert policy["require_cycles"] is False


def test_nr_is_reserved() -> None:
    try:
        cycles_policy_for_mode("nr")
        raise AssertionError("nr must be rejected")
    except ValueError as exc:
        assert str(exc) == NR_RESERVED_MESSAGE


def test_1080p_balanced_floor() -> None:
    assert ngx_min_render_size(1920, 1080, "BALANCED") == (960, 540)
    assert validate_render_percentage(1920, 1080, 25, "BALANCED") is not None
    assert validate_render_percentage(1920, 1080, 50, "BALANCED") is None
    assert recommended_percentage("BALANCED") == 58


def test_ultra_performance_allows_third() -> None:
    assert ngx_min_render_size(1920, 1080, "ULTRA_PERFORMANCE") == (640, 360)
    assert validate_render_percentage(1920, 1080, 34, "ULTRA_PERFORMANCE") is None


def test_parse_fg_multiplier() -> None:
    assert parse_fg_multiplier(None) == 0
    assert parse_fg_multiplier("2x") == 2
    assert parse_fg_multiplier("4") == 4
    try:
        parse_fg_multiplier("7x")
        raise AssertionError("7x must be rejected")
    except ValueError as exc:
        assert "2x..6x" in str(exc)
    assert RR_FG_CREATE_FAILED == "RR+FG 创建失败"
    assert "未接线" not in RR_FG_CREATE_FAILED
    assert "RrFgPipeline" not in RR_FG_CREATE_FAILED


def test_resolve_filename_template() -> None:
    assert DEFAULT_FILENAME_TEMPLATE == "{job}_{mode}_v{version}"
    assert job_token_from_blend(r"F:\Films\5.render_material_remaster.blend") == "5.render_material_remaster"
    mov = resolve_filename_template(
        "{job}_{mode}_v{version}",
        job="5.render_material_remaster",
        mode="rr",
        version=2,
        format="prores_422hq",
    )
    assert mov == "5.render_material_remaster_rr_v2.mov"
    seq = resolve_filename_template(
        "{job}_{mode}_v{version}",
        job="shot",
        mode="sr",
        version=1,
        format="exr",
    )
    assert seq == "shot_sr_v1_{frame:04d}.exr"
    seq_frame = resolve_filename_template(
        "{job}_{mode}_v{version}_{frame}",
        job="shot",
        mode="sr",
        version=3,
        format="png_16",
    )
    assert seq_frame == "shot_sr_v3_{frame:04d}.png"
    try:
        resolve_filename_template("   ", job="shot", mode="sr", version=1, format="prores_422hq")
        raise AssertionError("empty template must fail")
    except ValueError as exc:
        assert str(exc) == "文件名模板为空"
    try:
        resolve_filename_template("{job}_{foo}", job="shot", mode="sr", version=1, format="prores_422hq")
        raise AssertionError("unknown placeholder must fail")
    except ValueError as exc:
        assert "无法解析" in str(exc)
    try:
        resolve_filename_template("{job}_{mode}_v{version}", job="", mode="sr", version=1, format="prores_422hq")
        raise AssertionError("missing {job} must fail")
    except ValueError as exc:
        assert "{job}" in str(exc)


def test_default_leaves_blend_percentage() -> None:
    pct, reason = resolve_render_percentage(None, "BALANCED")
    assert pct is None
    assert reason == "blend"
    pct, reason = resolve_render_percentage(None, "BALANCED", legacy_scale=True)
    assert pct == recommended_percentage("BALANCED")
    assert reason == "legacy_quality_scale"
    pct, reason = resolve_render_percentage(57, "BALANCED")
    assert pct == 57
    assert reason == "explicit"
    pct, reason = resolve_render_percentage(58, "BALANCED", native_res_buffers=True, mode="rr")
    assert pct == 100
    assert reason == "rr_native_output"


def test_dual_pass_only_when_native_and_scaled_sr() -> None:
    assert wants_dual_pass(native_res_buffers=False, mode="sr", percentage=58) is False
    assert wants_dual_pass(native_res_buffers=True, mode="sr", percentage=58) is True
    assert wants_dual_pass(native_res_buffers=True, mode="sr", percentage=100) is False
    assert wants_dual_pass(native_res_buffers=True, mode="rr", percentage=58) is False
    assert "渲染" in phase_label("beauty", 830, 840)
    dual_bytes = estimate_window_frame_bytes(1114, 626, 1920, 1080, dual_pass=True)
    single = estimate_window_frame_bytes(1114, 626, 1920, 1080, dual_pass=False)
    assert dual_bytes > single


def test_ram_window_budget_90_or_1g() -> None:
    sixteen = 16 * 1024**3
    budget = ram_window_budget(sixteen)
    assert DEFAULT_RAM_FILL_PERCENT == 90
    assert DEFAULT_RAM_RESERVE_BYTES == 1024**3
    assert budget == min(sixteen * 90 // 100, sixteen - 1024**3)
    assert budget == sixteen * 90 // 100
    two = 2 * 1024**3
    assert ram_window_budget(two) == two - 1024**3
    tiny = 512 * 1024**2
    assert ram_window_budget(tiny) == tiny * 90 // 100


def test_plan_ram_windows_spans_two() -> None:
    # 10 frames, 5 MB each, 20 MB budget → 4 frames/window → ≥2 windows.
    windows = plan_ram_windows(1, 10, 5 * 1024**2, 20 * 1024**2)
    assert len(windows) >= 2
    assert windows[0][0] == 1
    assert windows[-1][1] == 10
    covered: list[int] = []
    for start, end in windows:
        covered.extend(range(start, end + 1))
    assert covered == list(range(1, 11))
    forced = plan_ram_windows(1, 6, 1, 10**12, max_frames=3)
    assert forced == [(1, 3), (4, 6)]


def test_estimate_exr_half_1080p() -> None:
    assert EXR_COLOR_DEPTH == "16"
    size = estimate_exr_bytes(1920, 1080, mode="sr", half=True)
    assert 1 * 1024**2 <= size <= 20 * 1024**2
    rr = estimate_exr_bytes(1920, 1080, mode="rr", half=True)
    assert rr > size


def test_user_error_message_strips_json_and_english() -> None:
    blob = (
        '{"event": "error", "message": "drive R: is not mounted; Stage 3 will not create a RAM disk", '
        '"missing": false} drive R: is not mounted; Stage 3 will not create a RAM disk'
    )
    short = user_error_message(blob)
    assert short == "R: 未挂载"
    assert "{" not in short
    assert "not mounted" not in short
    assert "Stage 3" not in short
    assert user_error_message('{"event":"error","message":"drive S: is not mounted"}') == "S: 未挂载"
    assert user_error_message("未找到卷标 DLSSRAM 的 RAM 盘。Stage 3 禁止自动建盘。") == "未找到 DLSSRAM"
    assert user_error_message("R: 未挂载") == "R: 未挂载"
    assert user_error_message("请安装 ImDisk Toolkit") == "请安装 ImDisk Toolkit"


def test_fullres_sr_uses_dlaa() -> None:
    assert quality_mode_for_buffer("BALANCED", 1920, 1080, 1920, 1080) == "DLAA"
    assert quality_mode_for_buffer("BALANCED", 1114, 626, 1920, 1080) == "BALANCED"
    assert quality_mode_for_buffer("BALANCED", 1920, 1080, 1920, 1080, mode="rr") == "BALANCED"


if __name__ == "__main__":
    test_rr_turns_off_cycles_denoiser()
    test_sr_does_not_force_cycles_denoiser()
    test_nr_is_reserved()
    test_1080p_balanced_floor()
    test_ultra_performance_allows_third()
    test_parse_fg_multiplier()
    test_resolve_filename_template()
    test_default_leaves_blend_percentage()
    test_dual_pass_only_when_native_and_scaled_sr()
    test_ram_window_budget_90_or_1g()
    test_plan_ram_windows_spans_two()
    test_estimate_exr_half_1080p()
    test_user_error_message_strips_json_and_english()
    test_fullres_sr_uses_dlaa()
    print("test_session_config ok")
