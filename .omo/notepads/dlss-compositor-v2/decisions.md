# Decisions — dlss-compositor-v2

## [2026-06-02] From Plan Interview

- Jitter base: 8 (DLSS §3.7.1.1)
- Communication: Flutter native plugin (MethodChannel + EventChannel + FFI shared memory)
- Branch: v2/main greenfield, keep only DLSS/ submodule
- Video output: ProRes 422 HQ + EXR; LDR also gets PNG 16-bit
- Tests: Tests-after + mandatory Agent QA scenarios
- T25 CAS removed (DLSS SR produces sharp output natively per §3.11)
- Tonemap: LDR -> sRGB EOTF only; HDR -> ACES Filmic Narkowicz + PQ ST.2084
- DNxHR/H264/etc: enum placeholder only, EncoderRegistry NOT registered
- Camera cut: matrix discontinuity primary + MV fallback; NOT cut_after
- resolution_percentage: handler transparent save->override->restore
- Apply Settings = Configure All Passes (merged; user sees only Apply Settings)
