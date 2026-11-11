# DLSS 310.9.16 upgrade notes

Updated locally on 2026-11-02:

- SDK submodule: `v310.6.0` (`d1bef2006b41eefd9d44b0a05f123993f3acbf3c`) -> `v310.9.16` (`9981c6350dc58c0ce21f024fd65e31e851c31689`).
- Windows x64 SR / RR / FG runtime DLLs in `resources/`: `310.7.129.0` -> `310.9.16.0`, copied from `DLSS/lib/Windows_x86_64/rel/`.
- This public SDK upgrade did not include NR. The separate development-only NR DLL was subsequently updated to NVIDIA's original `310.9.16.0` from the [Magpie v0.6.11 optional archive](https://github.com/SAOG0721/Magpie/releases/tag/v0.6.11-experimental). Packaging still includes only the three public SR / RR / FG DLLs.

## Official release history since the previous SDK pin

Dates below follow the upstream release pages (UTC).

| SDK | Release date | Published changes |
| --- | --- | --- |
| [310.7.0](https://github.com/NVIDIA/DLSS/releases/tag/v310.7.0) | 2026-06-23 | Improved resource/frame tracking in Nsight Graphics and Nsight Systems; bug fixes and stability improvements. |
| [310.9.1](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1) | 2026-09-08 | Added DLSS Ray Reconstruction Transformer Mode, Preset F; bug fixes and stability improvements. |
| [310.9.16](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16) | 2026-10-16 | Bug fixes and stability improvements; no individual fixes are listed. |

## Confirmed source changes

Compared with `v310.9.1`, the only changed public header is `include/nvsdk_ngx_defs_dlssg.h`. It adds `NVSDK_NGX_DLSSG_Parameter_LiteCandidateBlendingEnabled`, a create-time boolean enabling a lighter final blending mode for frame generation. The compositor does not set this new option. SR, RR and FG binaries and NGX libraries also changed; binary hashes alone do not identify specific algorithm or image-quality fixes.

Compared with the previous local SDK, `v310.6.0`:

- RR Preset F changes from an unused slot to the default RR2 model in `nvsdk_ngx_defs_dlssd.h`.
- Vulkan resource declarations move into `nvsdk_ngx_defs_vk.h`; the NGX handle becomes opaque.
- Helper headers are separated by API. `nvsdk_ngx_helpers_dlssd.h` is now a deprecated wrapper for `nvsdk_ngx_helpers_dlssd_d3d.h`. The shared `NGX_DLSSD_GET_OPTIMAL_SETTINGS` helper remains in that D3D-named header, so `dlss_rr.cpp` includes it directly alongside the Vulkan evaluation helper. The renderer continues to use Vulkan.
- New SR/RR responsivity-mask parameters, SR debug-overlay parameters and optional CUDA header-only loader headers are present. This update does not add compositor controls for them.
- The FG and RR PDF guides have changed since `v310.6.0`. Refer to the PDFs under `DLSS/doc/` for the current SDK; the existing Markdown mirrors in this directory retain their earlier snapshot.

## Verification

These are historical results from the SDK upgrade. The maintainer removed the automated test suites later on 2026-11-02; the test targets and sources named below are no longer maintained.

- Release builds passed for `dlss-compositor-cli`, `dlss_engine_tests` and `dlss_cli_tests`.
- All three copied runtime DLLs report `310.9.16.0`; SHA-256 matches between the SDK release bundle, `resources/` and the built executable directory.
- NGX initialization/capability test passed on the local RTX GPU.
- SR creation and five-frame evaluation, FG creation/destruction and 2x interpolation passed. Both CLI missing-input tests passed in the engine and standalone CLI test targets (seven focused CTest entries passed).
- Non-GPU engine tests: 117 of 123 test cases passed (1,318 of 1,324 assertions). Six assertions failed in unchanged test sources: three scalar ACES/PQ expectations in `test_tonemap_pass.cpp`, and three RPC expectations in `test_rpc_dispatcher.cpp` (`checkReady` after `setTask`, state after `startTask`, and empty preview metadata). An old-SDK baseline was not rebuilt; this is not a fully passing regression suite.
- Full RR rendering, complete export flows and image-quality comparisons were not run.

The first restricted-process test attempts stalled during NGX initialization and were stopped. The reported runtime results are from normal-user execution. The local CMake cache also needed its stale WindowsApps PowerShell alias replaced with an executable PowerShell path for vcpkg's post-build step; no machine-wide setting was changed.

Previous runtime DLLs and their SHA-256 manifest are retained locally under `build/sdk-upgrade/<timestamp>/runtime-backup/` and the adjacent `manifest.json` (ignored build artifacts).
