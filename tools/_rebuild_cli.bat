@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d F:\Projects\GitRepos\dlss-compositor
if not exist build\x64-windows-msvc-release cmake --preset x64-windows-msvc-release
cmake --build build\x64-windows-msvc-release --config Release --target dlss-compositor-cli --parallel
