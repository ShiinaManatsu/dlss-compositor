## [2026-05-15] Session ses_1d4dce9baffeI73Y90aKok0v7r — Pre-execution Analysis

### CRITICAL: State Management Mismatch
- AGENTS.md explicitly says: "State: Context + useReducer only — NEVER Redux or Zustand."
- Plan T7/T8 references "zustand slices" — IGNORE. Use Context + useReducer per AGENTS.md.
- Three existing stores: config-store.ts, processing-store.ts, banner-store.ts.
- Add SR/FG/Common state to config-store.ts using the existing Context+useReducer pattern.

### T13 Root Cause IDENTIFIED (pre-diagnosis)
- gui/electron/main.ts:27: if (event?.event === 'banner') -- looks for event.event
- Plan 1 T4 (dlss-pipeline-fix) emits: {"type":"banner",...} -- the field is "type" not "event"
- Fix: change event?.event === 'banner' to event?.type === 'banner'
- Also: banner goes to stdout (via globalNdjsonEmitter) -- correct stream is stdout -- NO stream issue
- Also: progress-parser.ts parseNdjsonLine likely returns whatever the JSON is -- verify it doesn't filter

### Channel Naming Convention (for T1 and T2)
- Combined → R, G, B, A channels (Cycles default RGBA)
- Depth → Z channel
- Motion Vectors → separate sibling file: <frame>.mv.exr containing R (X), G (Y)
- Standard EXR multi-part detection: Imf::MultiPartInputFile::parts() > 1

### Key File Facts
- gui/electron/main.ts:20-33 — probeNgxBanner (BROKEN — schema mismatch)
- gui/electron/settings-store.ts — uses electron-store (writes to userData) → T11 replaces
- gui/src/components/ExePathConfig.tsx — exists, must be DELETED in T10
- gui/src/components/BasicSettings.tsx — exists, must be DELETED in T7 (replaced by SR/FG/Common)
- src/pipeline/sequence_processor.cpp — extractTrailingFrameNumber at line ~85

### Dependency Execution Order
Wave 1 (parallel): T5 (quick), T12 (quick), T10 (TS refactor), T1 (Blender), T2 (C++ EXR)
  → T1 and T2 use same channel names (specified in plan), can truly parallel
Wave 2 (after Wave 1): T3 (inspect-exr + IPC), T11 (settings persistence), T13 (DLSS fix)
Wave 3 (after Wave 2): T4 (drop camera.json resolution), T6 (ResolutionPanel), T14 (FFmpeg), T15 (ProcessingView)
Wave 4 (sequential): T7 (SR/FG/Common split), then T8 (Advanced panel)
Wave 5 (parallel): T9 (Playwright), T16 (Docs)

### Codebase Conventions
- TypeScript + React 18, Electron 28, Tailwind CSS
- Default export PascalCase components
- window.dlssApi.* (NOT window.electronAPI — that was dlss-pipeline-fix convention)
- Playwright serial only (workers: 1)
- No zustand, no styled-components, no CSS modules — only Tailwind + Context+useReducer
