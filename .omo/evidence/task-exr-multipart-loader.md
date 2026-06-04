# Task EXR Multipart Loader Evidence

## Scenario
Blender multilayer EXR from `E:\New Folder(1)` is multipart. Python OpenEXR confirmed `0001.exr` has 3 parts:
- part 0: name empty, channels `RGBA`
- part 1: name `Z`, channels `Z.V`
- part 2: name `Vector`, channels `Vector.X`, `Vector.Y`, `Vector.Z`

## Fix
`src/io/frame_prefetcher.cpp` now uses `Imf::MultiPartInputFile` + `Imf::InputPart` to read all parts, while retaining the legacy `Imf::InputFile` single-part fallback.

## Verification
Command:
```powershell
$env:DLSS_COMPOSITOR_LOG='debug'
.\build\x64-windows-msvc-release\Release\dlss-compositor-cli.exe --config "E:\New Folder(1)\dlssconfig.json" --output-dir ".omo\evidence\user-config-real-pipeline-multipart" --output-format exr --output-template frame_####.exr --events stderr
```

Result: `EXIT=0`, 23 output EXR files generated.

Representative loader log:
```text
[FramePrefetcher] EXR E:\New Folder(1)\0001.exr parts=3 color=true depth=true vector=true
```
This appeared for all frames `0001.exr` through `0023.exr`, proving the loader no longer falsely treats the Blender EXRs as RGBA-only.

Output sample:
- `.omo/evidence/user-config-real-pipeline-multipart/frame_0023.exr`
- dataWindow: `(0, 0) - (1919, 1079)`
- channels: `A/B/G/R`
- size: `4035932`

Additional checks:
- CLI build succeeded: `dlss-compositor-cli`, `dlss_cli_tests`
- `dlss_cli_tests.exe`: all tests passed
- `flutter analyze`: no issues
- focused Flutter tests: all passed
- Flutter integration test: all passed
- `git diff --check`: no whitespace errors
