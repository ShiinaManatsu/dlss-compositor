# Running Blender Extension Tests

## Prerequisites

Python 3.11+ with:
- `pytest` (`pip install pytest`)
- bpy-stubs: installed at `C:\Python313\Lib\site-packages\bpy-stubs\` (user's machine)
- OR `fake-bpy-module`: `pip install fake-bpy-module` (fallback)
- `jsonschema`: `pip install jsonschema` (for schema validation)

## Run All Tests

From the repo root:
```bash
python -m pytest blender/tests/ -v
```

## Run Specific Test Files

```bash
python -m pytest blender/tests/test_register.py -v
python -m pytest blender/tests/test_jitter.py -v
python -m pytest blender/tests/test_handlers.py -v
python -m pytest blender/tests/test_quality_apply.py -v
python -m pytest blender/tests/test_dlssconfig_writer.py -v
python -m pytest blender/tests/test_vector_pass_sanity.py -v
python -m pytest blender/tests/test_integration.py -v
python -m pytest blender/tests/cross_check_constants.py -v
```

## CI Note

Blender tests are **not** in the GitHub Actions CI workflow (Blender is too heavy to install on runners). Run locally before pushing.

## bpy Mock Strategy

Tests use one of three approaches (in priority order):
1. `bpy-stubs` — type stubs at `C:\Python313\Lib\site-packages\bpy-stubs\`
2. `fake-bpy-module` — lightweight bpy mock (`pip install fake-bpy-module`)
3. `unittest.mock.MagicMock` — minimal runtime mock (always available)

The `conftest.py` sets up whichever is available automatically.
