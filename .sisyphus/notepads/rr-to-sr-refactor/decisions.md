# Decisions — rr-to-sr-refactor

## 2026-04-15 — Session ses_26eaf744cffeWPi53ue6j3Fq8a (Atlas)

- RR completely removed — no optional RR mode
- SR default preset: L (user explicit requirement)
- GBuffer hints (albedo, normals, roughness) are OPTIONAL for SR — VK_NULL_HANDLE = skip silently
- specularAlbedo REMOVED from DlssSRFrameInput (SR doesn't use it)
- Pipeline renames: processDirectoryRRFG → processDirectorySRFG
- Log prefix [RRFG] → [SRFG]
- FG logic COMPLETELY UNTOUCHED
- PQ transport COMPLETELY UNTOUCHED
- EXR reader/writer COMPLETELY UNTOUCHED
