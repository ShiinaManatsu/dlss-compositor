# Build Guide

This guide explains how to build DLSS Compositor from source on Windows.

## Prerequisites

- **Visual Studio 2022**: Install the "Desktop development with C++" workload. Ensure the MSVC v143 toolset is selected.
- **CMake 3.20+**: Available from [cmake.org](https://cmake.org/download/).
- **Vulkan SDK 1.3+**: Download the installer from [vulkan.lunarg.com](https://vulkan.lunarg.com/).
- **NVIDIA GPU**: RTX series (Turing architecture or newer) required for DLSS Ray Reconstruction.
- **NVIDIA Driver**: Version 520 or newer.

## Build Steps

### 1. Clone the Repository
```powershell
git clone <repository_url>
cd dlss-compositor
```

### 2. Initialize Submodules
The DLSS SDK is included as a git submodule.
```powershell
git submodule update --init --recursive
```

### 3. Configure with CMake
You must provide the path to the DLSS SDK using the `DLSS_SDK_ROOT` variable. If you initialized the submodules, this will be the `DLSS` directory.
```powershell
cmake -B build -G "Visual Studio 17 2022" -DDLSS_SDK_ROOT=DLSS
```

### 4. Build
```powershell
cmake --build build --config Release
```

### 5. Run Tests
Verify the build by running the unit tests and a DLSS availability check.
```powershell
ctest --test-dir build -C Release
build\Release\dlss-compositor.exe --test-ngx
```

## Troubleshooting

### "DLSS_SDK_ROOT must be set"
Ensure you passed `-DDLSS_SDK_ROOT=DLSS` (or the correct path to your DLSS SDK) to the `cmake -B build` command.

### "Vulkan SDK not found"
Make sure the Vulkan SDK is installed and the `VULKAN_SDK` environment variable is set. You may need to restart your terminal after installation.

### "nvngx_dlssd.dll not found"
The build process automatically copies this DLL from the DLSS SDK to the `build\Release` directory. If it's missing, manually copy `DLSS\lib\Windows_x86_64\rel\nvngx_dlssd.dll` to the directory containing `dlss-compositor.exe`.

### "DLSS-RR not available"
Check that your GPU is an NVIDIA RTX card and your drivers are up to date (520+). DLSS Ray Reconstruction specifically requires an RTX GPU.
