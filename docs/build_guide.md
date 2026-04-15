# Build Guide

This guide explains how to build DLSS Compositor from source on Windows.

## Prerequisites

- **Visual Studio 2022**: Install the "Desktop development with C++" workload. Ensure the MSVC v143 toolset is selected.
- **CMake 3.20+**: Available from [cmake.org](https://cmake.org/download/).
- **Vulkan SDK 1.3+**: Download the installer from [vulkan.lunarg.com](https://vulkan.lunarg.com/).
- **NVIDIA GPU**: RTX series (Turing architecture or newer) required for DLSS Super Resolution.
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

## Building the GUI

The DLSS Compositor GUI is built with Electron and React.

### Prerequisites
- **Node.js 18+ LTS**: Required for building the GUI.

### Build Steps

1. **Install dependencies**:
   ```powershell
   cd gui
   npm install
   ```

2. **Build the GUI**:
   ```powershell
   npm run build
   ```

3. **Development Mode**:
   Launch the GUI with hot-reload for development.
   ```powershell
   npm run dev
   ```

## Creating a Release Package

Once the C++ executable and the GUI are built, you can generate a complete release package using the provided PowerShell script.

1. **Ensure C++ Release build is complete.**
2. **Run the packaging script**:
   ```powershell
   .\scripts\package-release.ps1
   ```

### Script Arguments:
- `-SkipGuiBuild`: Skip the GUI build step if you've already run `npm run build`.
- `-Version "x.x.x"`: Manually specify the version number for the package.

The script will generate:
- `dist/dlss-compositor-v*.zip`: Contains the CLI tool, GUI, and all required DLLs/shaders/LUTs.
- `dist/dlss-compositor-blender-v*.zip`: The Blender extension for easy installation.

## Troubleshooting

### "DLSS_SDK_ROOT must be set"
Ensure you passed `-DDLSS_SDK_ROOT=DLSS` (or the correct path to your DLSS SDK) to the `cmake -B build` command.

### "Vulkan SDK not found"
Make sure the Vulkan SDK is installed and the `VULKAN_SDK` environment variable is set. You may need to restart your terminal after installation.

### GUI: "npm install" fails
Check your Node.js version (`node -v`). It must be version 18 or newer.

### "nvngx_dlssd.dll not found"
The build process automatically copies this DLL from the DLSS SDK to the `build\Release` directory. If it's missing, manually copy `DLSS\lib\Windows_x86_64\rel\nvngx_dlssd.dll` to the directory containing `dlss-compositor.exe`.

### "DLSS-SR not available"
Check that your GPU is an NVIDIA RTX card and your drivers are up to date (520+). DLSS Super Resolution specifically requires an RTX GPU.
