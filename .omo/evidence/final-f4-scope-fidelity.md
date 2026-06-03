# F4 RE-AUDIT — Scope Fidelity Check After Electron/Node Cleanup

Working directory: `D:\Github\dlss-compositor-opus-rework`  
Branch under audit: `v2/main`  
Audit focus: confirm deleted `gui/dist-electron/` and `gui/node_modules/` residue is gone, then re-check codec/scope fidelity.

## 1. Required files

### C++ required files

Command list contained 25 required C++/shader files. Result: **25/25 present**.

Missing C++ files: **0**.

### Blender / Flutter required files

Required Blender task surface from the v2 plan remains present: **12/12 present**.  
Required Flutter task surface remains present: **8/8 present**.

Tracked inventory cross-check:

- `git ls-files blender/`: 25 tracked Blender/add-on/test files
- `git ls-files gui/lib/`: 9 tracked Flutter source files

## 2. Electron/Node residue

Filesystem checks:

```text
Test-Path gui/dist-electron = False
Test-Path gui/node_modules  = False
```

Git-tracked check:

```text
git ls-files gui/ | Select-String "electron|node_modules" = 0
```

Electron/Node residue: **CLEAN**.

## 3. Scope creep checks

Source-only checks under `src/`:

```text
d3d12|D3D12                 = 0
NVSDK_NGX_NIS|NIS_          = 0
```

DX12/NIS scope creep: **0 found**.

## 4. EncoderRegistry scope

`src/io/async_writer.cpp` actual registrations:

```text
178: r.registerEncoder(OutputFormat::ExrLinear,   &encodeExrLinear);
179: r.registerEncoder(OutputFormat::Png16,       &encodePng16Stub);
180: r.registerEncoder(OutputFormat::ProRes422HQ, &encodeProRes422HQStub);
```

EncoderRegistry: **3 registered** (`ExrLinear`, `Png16`, `ProRes422HQ`).  
Note: textual search also matches the `EncoderRegistry::registerEncoder` function definition; actual registration calls are exactly 3.

## 5. DLSS version

```text
git -C DLSS describe --tags = v310.6.0
```

DLSS version: **v310.6.0**.

## 6. Schema duplication

```text
Get-ChildItem -Recurse blender/ -Filter *.schema.json = 0
```

Schema not duplicated: **clean**.

## 7. GUI dropdown scope

Read: `gui/lib/widgets/task_config_panel.dart`.

Relevant definitions:

- `enum OutputFormat { png16, exrLinear, prores422hq }`
- Labels:
  - `PNG (16-bit)`
  - `EXR (scene-linear)`
  - `ProRes 422 HQ`
- Values:
  - `png_16`
  - `exr`
  - `prores_422hq`

Availability logic:

```dart
if (isHdr) {
  return [OutputFormat.exrLinear, OutputFormat.prores422hq];
} else {
  return [OutputFormat.png16, OutputFormat.exrLinear, OutputFormat.prores422hq];
}
```

Dropdown items are built only from that filtered `formats` list:

```dart
items: formats
    .map((f) => DropdownMenuItem(value: f, child: Text(f.label)))
    .toList(),
```

Forbidden codec check in this file: no `DNxHR`, `H264`, `H265`, `AV1`, `AVIF`, `JPEG`, or `WebP` appears in dropdown value/child strings; grep found no forbidden codec names in the file.

GUI dropdown: **LDR=3, HDR=2, no forbidden codecs**.

## 8. RPC codec list

Read: `src/rpc/dispatcher.cpp`.

`handleGetStatus()` returns:

```cpp
{"video_encoders",      nlohmann::json::array({"prores_ks"})},
{"export_formats_ldr",  nlohmann::json::array({"png_16", "exr", "prores_422hq"})},
{"export_formats_hdr",  nlohmann::json::array({"exr", "prores_422hq"})},
```

RPC codec list: **correct**.

## 9. SR pass forbidden flags / constants

Read and grep: `src/dlss/dlss_sr.cpp`.

```text
InSharpness = 0
DepthInverted = 0
33.3f = 0
```

SR pass: **compliant**. There is no `InSharpness`, no `DepthInverted` assignment/comment, and frame time is supplied through `in.frameTimeDeltaMs` rather than hardcoded `33.3f`.

## 10. Verification notes

- This audit only updates `.omo/evidence/final-f4-scope-fidelity.md`; no production source code was changed.
- LSP diagnostics are not applicable to the Markdown evidence file in this workspace.
- Build is not applicable for the evidence-only audit update.

## Final output

```text
Required C++ Files: [25/25 present]
Required Blender Files: [12/12 present]  
Required Flutter Files: [8/8 present]
Electron/Node residue: [CLEAN]
DX12/NIS scope creep: [0 found]
EncoderRegistry: [3 registered]
DLSS version: [v310.6.0]
Schema not duplicated: [clean]
GUI dropdown: [LDR=3, HDR=2, no forbidden codecs]
RPC codec list: [correct]

Tasks [42/42 compliant] | Scope Creep [CLEAN] | VERDICT: APPROVE
```
