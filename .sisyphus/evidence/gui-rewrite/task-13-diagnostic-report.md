# Task 13 Diagnostic Report — DLSS Detection Banner

Command run from repository root:

```powershell
build\Release\dlss-compositor.exe --test-ngx --progress-format json > tmp/diag-stdout.txt 2> tmp/diag-stderr.txt
Get-Content tmp/diag-stdout.txt
Get-Content tmp/diag-stderr.txt
```

## Findings

- Banner JSON stream: **stdout** (`tmp/diag-stdout.txt`).
- Human-readable NGX lines stream: **stderr** (`tmp/diag-stderr.txt`).
- Process exit: **clean**, exit code `0`.

## stdout

```json
{"dlss_dll_version":"310.5.3.0","dlssg_dll_version":"310.5.3.0","ngx_api_version_path":"primary","ngx_api_version_used":"0x15","ngx_sdk_version":"310.5.3","type":"banner"}
```

The event discriminator field is `"type"`, not `"event"`.

## stderr

```text
[NGX] SDK: 310.5.3 (compile-time)
[NGX] Engine: dlss-compositor 0.2.1
[NGX] DLSS-SR: available
[NGX] DLSS-FG: available, max-multi-frame=1
[NGX] Min driver: 470.0
[NGX] Loaded: nvngx_dlss.dll v310.5.3.0, nvngx_dlssg.dll v310.5.3.0
DLSS-SR available: true
```

PowerShell also wrapped the first stderr line in a `NativeCommandError` record while preserving process exit code `0`; the underlying process did not fail.
