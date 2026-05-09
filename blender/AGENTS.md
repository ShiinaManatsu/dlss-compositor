# Blender Extension — Knowledge Base

**Generated:** 2026-05-09
**Commit:** 13a1b50
**Branch:** opus-rework

## OVERVIEW

Blender 4.2+ extension (`dlss_compositor_aov`): configures render passes, applies Halton sub-pixel jitter via camera shift, exports camera.json for Frame Generation.

## STRUCTURE

```
blender/
└── dlss_compositor_aov/
    ├── __init__.py           # 464-line single-file extension
    ├── blender_manifest.toml # Extension metadata (Blender 4.2+ format)
    └── (no submodules — intentional flat)
```

## KEY RESPONSIBILITIES

- **Pass registration** (`dlsscomp.configure_passes` — `DLSSCOMP_OT_configure_passes` at `__init__.py:212`): enables Combined + Depth + Vector passes on active view layer. Creates compositor File Output node set to MultiLayer EXR.
- **Halton jitter** (handlers `save_original_shift` at `:50` + `apply_jitter` at `:62` + `restore_shift` at `:90`): applies sub-pixel jitter to `camera.shift_x/y` based on `8 × output_scale²` jitter phases. Stores values to module-global `_runtime_jitter` dict.
- **Camera data export** (`dlsscomp.export_camera` — `DLSSCOMP_OT_export_camera` at `:114`): writes `camera.json` with per-frame matrices + jitter_x/y read from `_runtime_jitter` (single source of truth since Phase 1).

## GLOBAL STATE (critical)

```python
_original_shift_x: float      # line 44 — saved camera shift_x snapshot
_original_shift_y: float      # line 45 — saved camera shift_y snapshot
_jitter_enabled: bool          # line 46 — guard for render_init → apply_jitter flow
_runtime_jitter: dict          # line 106 — {frame_num: (jitter_x, jitter_y)}
```

Accessed by `render_pre`/`render_post`/`render_cancel` handlers + export operator. All prefixed `_` (module-private).

## HANDLER LIFECYCLE (`__init__.py:416-420`)

```
render_init      → save_original_shift(scene)    # snapshot + clear _runtime_jitter
frame_change_pre → apply_jitter(scene)            # halton(f%phases, 2/3) - 0.5 → delta
                                                  # store (jx, jy) in _runtime_jitter[frame]
render_complete  → restore_shift(scene)           # revert shift to snapshot
render_cancel    → restore_shift(scene)           # also revert on Esc
```

All four handlers registered with `@bpy.app.handlers.persistent` so they survive scene reload.

## CONVENTIONS

- `snake_case` functions, `_leading_underscore` for module-private globals
- **No type hints** — Blender `bpy` API preference
- **No dependencies** beyond `bpy` + standard `json` + `pathlib`
- `bl_idname` uses `dlsscomp.*` prefix (lowercase)
- UI panel (`DLSSCOMP_PT_export_panel` at `:332`) inherits `bpy.types.Panel` in Render Properties category

## ANTI-PATTERNS

- ❌ **Don't compute Halton in export operator** — must read from `_runtime_jitter` dict. Keeps render-time and export-time values identical (Phase 1 fix). Fallback path at `:187-189` exists for "user never rendered" but dict is authoritative.
- ❌ **Don't register handlers without `@persistent`** — lost on scene load.
- ❌ **Don't forget `restore_shift` in both `render_complete` AND `render_cancel`** — otherwise camera is left with jitter offset after Esc.
- ❌ **Don't split into multiple files** — Blender 4.2 extension manifest expects flat single-entry `__init__.py`. Modular split is a trap (import path gotchas).

## DEBUGGING

- Blender Python console: `import dlss_compositor_aov; dlss_compositor_aov._runtime_jitter` to inspect state
- Headless test: `blender --background --factory-startup --python tests/blender/test_jitter_consistency.py`
