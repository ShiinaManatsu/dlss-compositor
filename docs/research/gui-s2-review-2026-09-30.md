# GUI alignment with the 2026-09-30 Spectrum 2 profile

Reviewed the Electron GUI against the current `spectrum2-ui-design` Windows
shell, code-template, typography, and layout-stability references.

## Corrections

- Replaced the route ToggleButtonGroup and internal-slot CSS overrides with
  the skill's React Aria Link navigation template. Provider routes local hrefs;
  only the exact current link announces `aria-current="page"`. Disabled
  project-dependent destinations remain visible. Icons, home logo and bottom
  toggle share one axis; selection and keyboard focus have separate treatments.
- Adapted the animated panel toggle and three-state theme utility from the
  bundled templates. Application utilities precede host captions. This product
  has no global search or centered document-title requirement, so the center
  region remains usable window drag space.
- Caption geometry correction: D contains 48x48 logical-unit caption buttons aligned
  to the top-right. They do not stretch to the 64-unit application title row;
  A and C keep their own vertical centering.
  `--caption-size` uses the S2 width token; Electron converts logical units to
  physical pixels for the current display DPI (48 at 100%, 60 at 125%, 72 at
  150%). Do not additionally multiply the CSS size by devicePixelRatio.
- Unified shell geometry with macro-defined 16-unit gutters, 20-unit surface
  padding, 64-unit header, and 72/240-unit navigation. Chrome uses `base`, page
  surfaces use `layer-1`, and preview feedback uses `layer-2`. Settings retains
  its full-size surface and bounds readable groups internally.
- Workbench inspectors remain docked and resizable when the content area is
  at least 920 wide and 420 high. Smaller areas expose the same controlled
  fields in an S2 Dialog. The preview stays mounted; edits and preferred panel
  width survive adaptation. The splitter uses the shared gutter size.
- Removed the old root `data-color-scheme="dark"` lock. Auto mode now follows
  changes to the system media preference; explicit choices remain persisted.
- Removed hardcoded black/white NR transport styling and the Slider output
  override. The native frame slider displays 1-based values, converted to the
  backend's 0-based frame index. Hidden transport controls are inert.
- Removed redundant idle instructions and replaced manual ETA typography
  with S2 text roles. Back uses in-app route history; ancestor breadcrumbs
  return to the project page.

## Typography policy

Installed versions: S2 1.6.0, React Aria Components 1.20.0, React Aria 3.51.0,
Vite 7.3.6, Electron 36.9.5. The project does not upgrade S2 for this change.

`scripts/vite-s2-css.ts` lowers generated macro CSS for the actual Electron
Chromium target in development. `scripts/vite-s2-fonts.ts` is an explicit,
product-owned dependency CSS policy: add Microsoft YaHei UI / Microsoft YaHei
before SimSun in simplified-Chinese font-family declarations, keeping Adobe
Han first. It covers compiled controls and application macros in dev/build;
it does not edit node_modules or override control slots, weights, or geometry.
Revalidate the policy when upgrading S2. Remote font stripping for existing
Latin faces remains a separate pre-existing build policy.

## Historical verification

The automated renderer harness used for this review was removed at the
maintainer's request on 2026-11-02. The results below describe the original
review, including fixture-backed production and Vite renderer runs; no Blender
or GPU export was started.

Coverage includes 1440x900, 1024x768, 1280x600, 800x600; both themes; expanded
and collapsed navigation; native link activation and current-page semantics;
equal route widths and logo/icon/toggle axes; transition interpolation;
keyboard/pointer split resizing; narrow inspector Dialog, Escape and return
focus; retained edits and decoded preview; three-state theme cycling and live
system preference changes; reduced-motion emulation; in-app Back; and no
unexpected horizontal overflow in the tested main regions.

Chromium CSS.getPlatformFontsForNode records actual glyph families alongside
computed stacks for headings, navigation, buttons, menu options, Dialog and
inputs. In this machine's captured runs, the Chinese samples rendered with
Microsoft YaHei UI, including the bold heading face.

Artifacts: `build/ui-smoke/` and `build/ui-smoke-dev/`, with screenshots and
`metrics.json`. Progress, DCP and error regressions were also recorded at the time.

These renderer checks do not establish real GPU/NR export behavior, UAC user
interaction, Windows Snap Layouts, multiple-monitor transitions or physical
touch input. Native window command wiring is retained; its OS behavior is not
claimed from screenshots. The host minimum remains 960x640; the 800x600 case
is an additional renderer stress check.
