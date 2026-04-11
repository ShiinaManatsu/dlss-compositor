# Issues — dlss-compositor

- [2026-04-11] `lsp_diagnostics` could not be executed for C++ files because `clangd` is not installed in the environment.
- [2026-04-11] Re-running `cmake -B build -G "Visual Studio 17 2022" -A x64 ...` failed because `build/` had been configured previously without an explicit platform; reusing the existing build directory with `cmake -B build ...` succeeded.
- [2026-04-11] `ctest --test-dir build --config Release` is not supported by the installed CTest; `ctest -C Release --test-dir build ...` is the compatible syntax on this machine.
- [2026-04-11] Motion-vector test expectations were stale after the `875c30a` correction; they needed pass-through X/Y and unit scale updates rather than source changes.
