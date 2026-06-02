# Issues — rr-to-sr-refactor

## 2026-04-15 — Session ses_26eaf744cffeWPi53ue6j3Fq8a (Atlas)

- LSP errors in C++ files are PRE-EXISTING (volk.h not found, NVSDK types unknown)
  These are expected — LSP doesn't have the correct include paths configured.
  The actual CMake build will succeed as CMake provides proper include paths.
  Do NOT attempt to fix these LSP errors — they are not caused by our changes.
