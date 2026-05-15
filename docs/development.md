# Development Guide

This document covers the development workflow for DLSS Compositor.

## Prerequisites
- Visual Studio 2022 (with C++ workload)
- CMake 3.20+
- Vulkan SDK 1.3+
- Node.js 18+ LTS
- Blender 4.2+ (for extension testing)

## Building the C++ CLI

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=DLSS
cmake --build build --config Release
```

## Running the GUI in Development

```powershell
cd gui
npm install
npm run dev
```

## VSCode F5 Workflow

Three debug configurations are provided in `.vscode/launch.json`:

1. **Launch GUI (Dev)** — Starts the Electron GUI in development mode with hot reload.
2. **Launch GUI (Packaged)** — Builds the packaged GUI and launches it.
3. **Debug C++ CLI** — Builds and attaches the MSVC debugger to the CLI executable.

Select the desired configuration in the Run and Debug panel (Ctrl+Shift+D) and press F5.

## Running Tests

```powershell
# C++ unit tests
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# GUI Playwright E2E tests
cd gui
npx playwright test
```

See the full development documentation at [docs/development.md](development.md) (enriched by T16).
