# Learnings — custom-scale-blender-ext-electron-gui

## [2026-04-12] Task 1: DLAA Quality Mode
- DLAA is already present in the NGX SDK enum, so the app-side change is just enum plumbing and string parsing/mapping.
- Defaulting AppConfig quality to MaxQuality aligns the CLI with the new quality-first behavior without changing any scale logic.
- The CLI help/error text should list DLAA first so users see the new denoise-only mode immediately.
