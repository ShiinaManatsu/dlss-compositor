# Phase 4 GUI Polish - Decisions

## 2026-05-07 Initial Decisions

### Wave Execution Strategy
- Wave A (Tasks 1 & 2): CLI changes, run in parallel
- Wave B (Tasks 3 & 4): GUI control additions, run in parallel  
- Wave C (Tasks 5, 6, 7): GUI integration, run in parallel (Task 5 depends on Wave A.2 conceptually, but since CLI is already done, can proceed)
- Wave D (Tasks 8, 9, 10): Task 8 depends on Task 7, Task 9 depends on Tasks 2+5; Task 10 is independent
- Wave E (Tasks 11, 12): Sequential, both depend on previous tasks
- Wave F (F1-F4): Final verification, all parallel

### Camera Auto-Discovery Location
CLI change should go in `src/main.cpp` (after CliParser::parse) or in `src/cli/cli_parser.cpp` (at the end of parse()). 
Recommend: at end of `CliParser::parse()` - after validation passes, check if cameraDataFile is empty + inputDir is not empty.

### NDJSON Implementation
- Use nlohmann::json (already in project)
- New `src/core/ndjson_emitter.h/cpp` with thread-safe emit
- `--progress-format <human|json>` flag with default 'human'
- Banner event goes out regardless of format

### GUI Progress Parser Replacement  
- GUI ALWAYS uses `--progress-format json` (adds to buildCliArgs)
- Old regex parser kept as fallback but primary path is NDJSON

### Build System
- C++ uses CMake, build with `cmake --build build --config Release`
- GUI uses Vite + Electron, `npm run build` from `gui/` directory
- Tests: `ctest --test-dir build` for C++, `npx playwright test` for GUI
