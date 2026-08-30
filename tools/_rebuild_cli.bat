@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build "F:/Projects/GitRepos/dlss-compositor/.claude/worktrees/clever-cray-7056de/build/x64-windows-msvc-release" --config Release --target dlss-compositor-cli --parallel
