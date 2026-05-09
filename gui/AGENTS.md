# DLSS Compositor — GUI Knowledge Base

**Generated:** 2026-05-09
**Commit:** 13a1b50
**Branch:** opus-rework

## OVERVIEW

Electron 28 + React 18 + Vite + electron-vite + Tailwind 3. Spawns `dlss-compositor.exe` as child process, parses NDJSON progress on stdout.

## STRUCTURE

```
gui/
├── electron/              # Main process (9 files)
│   ├── main.ts            # Window + IPC handlers + NGX banner probe
│   ├── preload.ts         # contextBridge → window.dlssApi
│   ├── cli-args.ts        # buildCliArgs(config) → spawn argv
│   ├── progress-parser.ts # NDJSON line parser (ProgressEvent + parseNdjsonLine + ProgressLineBuffer)
│   ├── process-manager.ts # spawn / kill child process
│   ├── settings-store.ts  # electron-store persistence (Settings)
│   ├── camera-detector.ts # findCameraJson + readSourceResolution
│   ├── preview-generator.ts # FFmpeg EXR→PNG conversion
│   └── ipc-channels.ts    # Channel name constants (IPC_CHANNELS)
├── src/                   # React renderer
│   ├── App.tsx            # Root component — ConfigProvider + BannerProvider + Layout
│   ├── components/        # 8 .tsx files
│   │   ├── Layout.tsx
│   │   ├── InputOutputPanel.tsx
│   │   ├── BasicSettings.tsx
│   │   ├── AdvancedSettings.tsx
│   │   ├── ResolutionPanel.tsx
│   │   ├── ProcessingView.tsx
│   │   ├── StatusBar.tsx
│   │   └── ExePathConfig.tsx
│   ├── state/
│   │   ├── config-store.ts    # Context + useReducer (ConfigState, ConfigAction)
│   │   ├── processing-store.ts
│   │   └── banner-store.ts
│   ├── hooks/
│   │   └── useProcessing.ts
│   ├── types/
│   │   ├── dlss-config.ts     # DlssConfig interface (30 fields), DEFAULT_CONFIG, Settings
│   │   └── window.d.ts        # DlssApi interface + global Window augmentation
│   └── ipc/
│       ├── channels.ts        # Duplicate of electron/ipc-channels.ts (cross-boundary)
│       └── index.ts
├── tests/                 # 11 Playwright .spec.ts files (serial only)
│   └── fixtures/
│       ├── with_camera_json/  # camera.json + 3 empty EXR stubs
│       └── without_camera_json/ # 2 empty EXR stubs
└── playwright.config.ts   # fullyParallel: false, workers: 1
```

## PROCESS MODEL

```
Main (Node) — electron/main.ts:35
    ├── spawn dlss-compositor.exe --progress-format json  (process-manager.ts:8)
    ├── parse NDJSON stdout → IPC events               (progress-parser.ts:32)
    ├── probeNgxBanner on window load                   (main.ts:20)
    └── IPC handlers: dialogs, settings, camera detect, preview gen (main.ts:81)
Preload — electron/preload.ts:3
    └── contextBridge.exposeInMainWorld('dlssApi', {…})   (preload.ts:3)
Renderer (React) — src/App.tsx:1
    ├── window.dlssApi.* typed via src/types/window.d.ts:5
    ├── Config state via Context + useReducer              (config-store.ts:146)
    └── Tailwind utility classes
```

## IPC CHANNELS

Defined in `electron/ipc-channels.ts:1-18` (duplicated at `src/ipc/channels.ts:1-18` for renderer imports):

| Channel | Direction | Handler |
|---------|-----------|---------|
| `dialog:openDirectory` | invoke | File open dialog |
| `dialog:openFile` | invoke | File open dialog with filters |
| `process:start` | send | spawn exe with config |
| `process:stop` | send | kill child process |
| `process:progress` | send→renderer | `{current, total}` |
| `process:frame-written` | send→renderer | `{path, n}` |
| `process:banner` | send→renderer | NGX capability banner |
| `process:error` | send→renderer | Error message string |
| `process:complete` | send→renderer | Exit with success |
| `process:log` | send→renderer | Passthrough log lines |
| `settings:get` | invoke | Load from electron-store |
| `settings:save` | invoke | Persist to electron-store |
| `settings:validate-exe-path` | invoke | exists + ngx probe |
| `camera:auto-detect` | invoke | findCameraJson + readSourceResolution |
| `frame:preview-png` | invoke | FFmpeg EXR→PNG |
| `count:exr-files` | invoke | `.exr` count in dir |

## NDJSON EVENT TYPES

From `electron/progress-parser.ts:19-25` — emitted by CLI with `--progress-format json`:

```
started        | frame_started  | frame_written
banner         | error          | complete
```

## CONVENTIONS

- **Components**: Default export `PascalCase`; props typed inline. `src/components/`
- **Hooks**: `useX` camelCase, imported from `src/hooks/`. Example: `useProcessing` in `src/hooks/useProcessing.ts`
- **State**: Context + useReducer only — **never** Redux or Zustand. Three stores: `config-store.ts` (DlssConfig), `processing-store.ts`, `banner-store.ts`
- **IPC**: Renderer (`src/`) never imports `fs`, `path`, `child_process` — all Node access via preload bridge. IPC channels declared once in `electron/ipc-channels.ts` and re-exported in `src/ipc/channels.ts` for TypeScript cross-boundary.
- **Styling**: Tailwind utility classes only. No `styled-components`, no CSS modules.
- **Build**: `electron-vite build` — electron-vite is the build tool (not vanilla vite/electron-builder).

## ANTI-PATTERNS

- Don't call `window.dlssApi.*` without the type — import from `window.d.ts`, which declares global `Window.dlssApi: DlssApi` at `src/types/window.d.ts:27-31`.
- Don't set `workers > 1` or `fullyParallel: true` in `playwright.config.ts` — Electron can only run one instance.
- Don't parse CLI stdout with regex — Phase 4 replaced `parseProgressLine` with NDJSON (`--progress-format json`). The old regex parser is deprecated but kept at `progress-parser.ts:9` for backward compat.
- Don't persist secrets via electron-store — it writes plaintext JSON to `%APPDATA%/dlss-compositor-gui/config.json`.
- Don't bypass preload — renderer is sandbox-enabled: `contextIsolation: true`, `nodeIntegration: false` (`main.ts:48-51`).

## TESTING

- **Run**: `npx playwright test` (or `npm run test:e2e`)
- **Config**: `playwright.config.ts:1-14` — `fullyParallel: false`, `workers: 1`, 30s timeout
- **Specs** (11): `app-launch`, `auto-detect`, `preview`, `resolution`, `ndjson-parser`, `statusbar`, `new-controls`, `real-processing`, `validation`, `settings-flow`, `screenshot-evidence`
- **Fixtures**: `tests/fixtures/with_camera_json/` (camera.json + 3 EXR stubs), `without_camera_json/` (2 EXR stubs)
- **Serial only** — Electron can't parallelize browser instances
