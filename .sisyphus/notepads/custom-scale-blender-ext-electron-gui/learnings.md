# Learnings — custom-scale-blender-ext-electron-gui

## [2026-04-12] Task 1: DLAA Quality Mode
- DLAA is already present in the NGX SDK enum, so the app-side change is just enum plumbing and string parsing/mapping.
- Defaulting AppConfig quality to MaxQuality aligns the CLI with the new quality-first behavior without changing any scale logic.
- The CLI help/error text should list DLAA first so users see the new denoise-only mode immediately.
## [2026-04-12] Task 6: Blender Extension
- Converted the AOV export preset into a Blender 5.x extension package under blender/dlss_compositor_aov/ with a manifest and package entrypoint.
- Removed the legacy bl_info block from the extension entrypoint and kept the register/unregister and __main__ behavior intact.
- Blender 5.1 headless test passed and reported the expected pass configuration output.
## [2026-04-12] Task 2: Float Scale CLI Parsing
- `--scale` now accepts floats via the existing `parseFloat()` helper, with range checks at CLI parse time instead of integer-only discrete values.
- Keeping `scaleExplicit` set when `--scale` is provided preserves existing override semantics while allowing backward-compatible integer input.
- The help text should advertise the float range clearly so users can combine fractional scaling with interpolation.
## [2026-04-12] Task 7: BlenderMCP Verification
- All 3 QA scenarios passed on first run with zero fixes needed — Task 6's implementation was clean.
- Blender 5.1.0 (hash adfe2921d5f3) headless test exits 0 and prints the expected "Passes configured" message, confirming register() and operator execution work correctly.
- Static analysis confirmed panel class has correct bl_space_type="PROPERTIES", bl_context="render", and references both operators in draw().
- The extension directory structure is correct: blender_manifest.toml with schema_version="1.0.0", __init__.py with no bl_info, and proper register/unregister functions.
- Evidence files saved to .sisyphus/evidence/task-7-{structure,headless,panel-visible}.txt.

## [2026-04-12] Task 3: Float Scale Resolution Calc + Pipeline Routing
- Defaulting `scaleFactor` to `0.0f` keeps FG-only runs on the existing interpolation path until `--scale` is explicitly provided.
- Fractional RR output sizing needs `std::round()` before snapping to even dimensions so NGX feature creation gets stable integer extents for scales like `1.5x`.
- Replacing the GUI scale combo with `ImGui::InputFloat` avoids broken integer index math and keeps custom float scales aligned with the CLI parser.
## [2026-04-12] Task 4: Stdout Flush
- `sequence_processor.cpp` now flushes `stdout` after every progress-emitting `std::fprintf(stdout, ...)` call so the Electron GUI can receive lines immediately from the child process.
- The flush coverage includes per-frame progress, RR/FG pipeline status, transport initialization, and writer lifecycle messages; `stderr` warnings were left untouched.
- Build verification succeeded with `cmake --build build --config Release`, and the source now has a 1:1 count of stdout progress prints and flushes.

## [2026-04-12] Task 5: Catch2 Tests
- Added focused CLI coverage for float scale parsing, explicit scale flag semantics, and case-sensitive DLAA quality parsing.
- --scale float tests should keep using Catch::Approx so 1.5/1.0/2.0 and integer-string input all validate without precision noise.
- The invalid-value cases are best captured as separate sub-blocks inside one [cli] test section, matching the existing compact style.

## [2026-04-12] Task 8: Documentation Update
<findings>
- Updated README.md and docs/usage_guide.md to reflect support for float scale factors (1.0-8.0) and DLAA quality mode.
- Updated Blender configuration instructions to reflect the new Blender 5.x Extension system (Install from Disk) replacing the previous script execution method.
- Added specific examples for common scale factors like 1.5x (720p to 1080p) and 1.0x (denoise-only).
- Maintained existing CLI examples while adding new ones to ensure backward compatibility in documentation tone.
</findings>

## [2026-04-12] Task 9: Electron Scaffold
- electron-vite requires config file named electron.vite.config.ts (NOT ite.config.ts — it explicitly rejects that name).
- When main and preload share the same outDir (e.g. dist-electron/), the preload build clears main's output. Fix: set emptyOutDir: false on the preload config.
- electron-vite with "type": "module" in package.json emits preload as .mjs; the main.ts preload reference must use preload.mjs accordingly.
- externalizeDepsPlugin() is required for main/preload configs to externalize Node.js built-in modules and npm dependencies.
- Tailwind v3 + PostCSS + autoprefixer work out of the box with electron-vite's renderer pipeline — no special configuration needed.
- electron-vite's process.env.ELECTRON_RENDERER_URL is the correct env var for dev server URL detection (not VITE_DEV_SERVER_URL).
- The oot: '.' in renderer config is needed when index.html is at project root (not in src/).
- Build produces: dist-electron/main.js (~1KB), dist-electron/preload.mjs (~0.1KB), dist/index.html + dist/assets/* (~225KB total).

## [2026-04-12] Task 10: Electron IPC + Process Management
- Duplicating IPC channel constants under both `gui/src/ipc/` and `gui/electron/` keeps renderer and main/preload bundles aligned without relying on cross-bundle runtime imports.
- A pure `gui/electron/cli-args.ts` helper makes `buildCliArgs()` independently testable from Node, which is useful before any renderer UI exists.
- Inspecting the built preload bundle is sufficient to confirm `window.dlssApi` exposes the expected bridge methods before wiring them into renderer components.
