## [2026-05-15] Session ses_1d4dce9baffeI73Y90aKok0v7r — Pre-execution Analysis

### Key Findings from Code Audit

**ngx_wrapper.cpp — Lines 239–251 (CURRENT PRESET CODE)**
The preset code already uses a pickPreset lambda with per-quality defaults (DLAA/Quality/Balanced/UltraQuality=K, Perf=M, UltraPerf=L).
BUG: All 6 slots resolve from the SAME global preset param — no per-quality override fields exist.
FIX: Add per-quality fields to AppConfig, update createDlssSR signature to accept the config.

**getDllFileVersion — ALREADY IMPLEMENTED**

gx_wrapper.cpp lines 75-110 already have getDllFileVersion(const wchar_t* dllName) working.
Lines 197-201: already logs "Loaded: nvngx_dlss.dll vX.Y.Z.W" via Log::info.
T4 needs to: add NDJSON banner event (not just Log::info), add API version fallback on init failure.

**DlssSRPreset::Auto = 0 — ALREADY EXISTS** in config.h line 19.

**CameraFrameData — ALREADY HAS position[3], forward[3]** in camera_data_loader.h lines 13-16.
CameraDataLoader::getFrame(int frameNumber) exists — uses string key via padFrameNumber().
No getFrameByIndex(int index) exists — need to add or use hasFrame/getFrame with frameNumber.

**createDlssSR signature** — read ngx_wrapper.h to confirm exact args before modifying.

**dlss_sr_processor.cpp:142** — evalParams.InFrameTimeDeltaInMsec = frame.frameTimeDeltaMs; (dynamic already)
Lines 143-144: InPreExposure = 1.0f, InExposureScale = 1.0f hardcoded — T2 must make configurable.

**sequence_processor.cpp InReset/reset** — Lines 882, 1333, 1486, 1573, 2021 use .reset field.
Line 882 region is likely the SR-only pipeline loop.

**File Conflict Warning**: T1+T2+T3 ALL touch config.h + cli_parser.cpp → RUN SEQUENTIALLY.

### Execution Order Decision
Run sequentially: T1 → T2 → T3 → T4 → T5 (avoid merge conflicts on shared files).
T1 commits first; T2 builds on T1's config.h additions; T3 adds camera cut fields last.

### Codebase Conventions (from AGENTS.md)
- m_ prefix for members, PascalCase classes, camelCase methods, snake_case files
- No using namespace std
- Error return: ool func(..., std::string& errorMsg) pattern
- No exceptions for expected failures
- NDJSON events via globalNdjsonEmitter().emitRaw() (thread-safe singleton)
- Log::info → stdout+log; Log::error → stderr+log
- Log::setQuietStdout(true) when progress-format=json (info goes to stderr)
- pragma comment(lib, "Version.lib") already in ngx_wrapper.cpp line 24
