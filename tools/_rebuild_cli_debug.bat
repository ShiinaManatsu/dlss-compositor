@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
cd /d F:\Projects\GitRepos\dlss-compositor
if exist build\x64-windows-msvc-debug\CMakeCache.txt if not exist build\x64-windows-msvc-debug\build-Debug.ninja rmdir /s /q build\x64-windows-msvc-debug
if not exist build\x64-windows-msvc-debug cmake --preset x64-windows-msvc-debug
cmake --build build\x64-windows-msvc-debug --config Debug --target dlss-compositor-cli --parallel
