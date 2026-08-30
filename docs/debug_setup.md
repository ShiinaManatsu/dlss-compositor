# Debug Setup — DLSS Compositor v2

## Overview

This repository supports a dual-debug workflow in VS Code:

- **Flutter/Dart debugging** for the GUI entry point in `gui/lib/main.dart`
- **C++ debugging** for both the standalone CLI and the Windows Flutter runner/plugin process

The recommended workflow is a single **compound launch configuration** that builds the Flutter Windows target, starts the Dart debugger, and then lets you attach the native C++ debugger to the running GUI process.

This document explains setup, daily usage, and the most common failure modes.

---

## Prerequisites

Before pressing `F5`, make sure the local machine is ready for both the Dart and native Windows toolchains.

### Required software

- **VS Code**
- **Flutter 3.22+**
- **Visual Studio 2022 Build Tools** (or VS2022 with C++ desktop workload)
- **CMake** available on `PATH`

### Recommended VS Code extensions

- `dart-code.dart-code`
- `dart-code.flutter`
- `ms-vscode.cpptools`
- `ms-vscode.cmake-tools`
- `twxs.cmake`

### Environment checks

Run these once from a terminal:

```powershell
flutter doctor
cmake --version
```

Expected state:

- `flutter doctor` is green for Windows desktop support
- `cmake` resolves without a PATH error
- the C++ toolchain is installed and discoverable by the Flutter Windows build

### Expected build outputs

This setup assumes the following output locations:

- CLI executable: `build/Release/dlss-compositor-cli.exe`
- Flutter Windows runner symbols/binaries: `gui/build/windows/x64/runner/Release/`
- Native CLI symbols/binaries: `build/Release/`

---

## Workspace setup

1. Open the repository root in VS Code.
2. Let VS Code detect the `.vscode/` workspace files.
3. If prompted, install the recommended extensions.
4. Ensure dependencies are restored for Flutter:

```powershell
cd gui
flutter pub get
```

If you prefer, open the repo from the root with:

```powershell
code .
```

The workspace is configured so CMake does **not** auto-configure on open. This avoids unexpected configure/build work when you only want to inspect or debug.

---

## F5 Flutter + C++ dual debug

Use the compound configuration named **Flutter + C++ (Plugin Debug)**.

### What happens when you start it

1. VS Code runs the `build-flutter` task:
   - `flutter build windows --debug`
   - working directory: `${workspaceFolder}/gui`
2. VS Code launches the Dart debugger with:
   - program: `gui/lib/main.dart`
   - cwd: `${workspaceFolder}/gui`
   - args: `--enable-vm-service`
3. VS Code prompts you to attach the C++ debugger to a running process.
4. Select **`dlss_compositor_gui.exe`** from the process picker.

### Why this is split into launch + attach

Flutter owns the GUI process lifecycle, so the native debugger is configured as an **attach** action instead of a fully native launch. This keeps the Dart and Windows native debugging flows compatible.

### Setting breakpoints

After attach succeeds, you can place breakpoints in both layers:

- Dart: `.dart`
- Native: `.cpp`, `.h`

Typical use cases:

- verify a button action in the GUI triggers the expected Dart code path
- confirm the platform bridge/plugin enters the intended C++ function
- inspect native state or COM interaction after a Dart-side event

---

## C++ CLI debug (standalone)

Use the **C++ Launch CLI** configuration when you want to debug the native executable without the Flutter GUI.

### Behavior

- pre-launch task: `build-cli`
- executable: `${workspaceFolder}/build/Release/dlss-compositor-cli.exe`
- args:
  - `--config`
  - `${workspaceFolder}/cli/tests/fixtures/sample-dlssconfig.json`
- environment:
  - `DLSS_COMPOSITOR_LOG=debug`

### When to use it

Use this mode for:

- validating pure C++ behavior
- stepping through configuration parsing
- reproducing logging or pipeline issues without the GUI
- reducing noise when the Flutter layer is not relevant

---

## Build tasks reference

Two workspace tasks are defined for debugger integration.

### `build-cli`

```powershell
cmake --build build --config Release --parallel
```

Use this when debugging or rebuilding the standalone CLI binary.

### `build-flutter`

```powershell
flutter build windows --debug
```

Use this before the compound GUI debug session so the Windows runner, plugin DLLs, and symbols are ready.

---

## Troubleshooting

### Symbols not loading

If the VS Code C++ debugger shows **Symbols not loaded**:

1. Confirm PDB files exist:

```powershell
Get-ChildItem build/Release/*.pdb
Get-ChildItem gui/build/windows/x64/runner/Release/*.pdb
```

2. Rebuild the corresponding target.
3. Confirm `symbolSearchPath` still includes both:
   - `${workspaceFolder}/gui/build/windows/x64/runner/Release`
   - `${workspaceFolder}/build/Release`

If symbols still do not load, close the debug session, rebuild, and attach again so the debugger reloads modules cleanly.

### Flutter Dart debugger not attaching

If Dart breakpoints stay unbound or the debugger fails to connect:

- verify `--enable-vm-service` remains in launch args
- verify `cwd` is `${workspaceFolder}/gui`
- run `flutter pub get`
- make sure Windows desktop support is enabled in Flutter

You can also retry with a clean terminal build:

```powershell
cd gui
flutter clean
flutter pub get
flutter build windows --debug
```

### C++ attach picker shows the wrong process

The attach step uses `${command:pickProcess}`. If multiple similar processes exist:

- start from a fresh debug session
- wait until the Flutter runner window appears
- select `dlss_compositor_gui.exe`
- avoid attaching to `code.exe`, `flutter_tester.exe`, or stale prior runs

### COM objects not resolving in C++

If plugin/native COM-related breakpoints or symbol lookups behave unexpectedly:

- confirm the plugin DLL and PDBs exist under `gui/build/windows/x64/runner/Release/`
- keep that folder in `symbolSearchPath`
- rebuild the Flutter Windows debug target after native code changes

### Pre-launch build task fails

If the task fails before the debugger starts:

- run the task manually from **Terminal → Run Task**
- inspect whether `flutter`, `cmake`, and the MSVC toolchain resolve on `PATH`
- fix the task failure first; launch configurations intentionally assume build tasks gate the session

---

## Environment variables

### `DLSS_COMPOSITOR_LOG=debug`

Used by the CLI launch configuration to enable verbose logging to stderr or console output while debugging.

### `VCPKG_ROOT`

If this project depends on vcpkg-based native dependencies, ensure `VCPKG_ROOT` points to a valid installation before configuring or rebuilding native code.

---

## Recommended workflow summary

### For GUI + plugin/native issues

1. Open the repo root in VS Code.
2. Choose **Flutter + C++ (Plugin Debug)**.
3. Press `F5`.
4. Pick `dlss_compositor_gui.exe` when prompted.
5. Debug across Dart and C++ breakpoints.

### For CLI-only native issues

1. Choose **C++ Launch CLI**.
2. Press `F5`.
3. Reproduce with `sample-dlssconfig.json`.
4. Inspect native logs and variables.

This split keeps the day-to-day debug flow fast while still supporting mixed-language investigations when needed.
