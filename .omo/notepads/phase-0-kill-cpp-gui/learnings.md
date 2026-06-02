# Learnings — phase-0-kill-cpp-gui

## [2026-05-07] Initial Setup

### Codebase structure
- `src/ui/` contains: app.cpp/h, image_viewer.cpp/h, settings_panel.cpp/h, .gitkeep
- `src/cli/`: cli_parser.cpp, cli_parser.h, config.h
- `src/gpu/`: vulkan_context.cpp/h, texture_pipeline.cpp/h, etc.
- `src/main.cpp`: main entry point
- `gui/electron/`: Electron main process TypeScript files
- `gui/src/`: Renderer process React/TS files

### Task 1 Audit Result (2026-05-07)
- ALL 4 flags safe to delete from C++; Electron does NOT call --gui/--test-gui/--test-load/--test-process
- Electron builds CLI args via buildCliArgs() in gui/electron/cli-args.ts (pure processing flags only)

### config.h GUI fields to remove (lines 68-71)
- `bool testGui = false;`
- `std::string testGuiLoad;`
- `bool testGuiProcess = false;`
- `bool launchGui = false;`

### cli_parser.cpp GUI code to remove
- `--gui` flag handler: lines 214-217
- `--test-gui` flag handler: lines 218-221
- `--test-process` flag handler: lines 222-225
- `--test-load` flag handler: lines 320-327
- printHelp() GUI flags: lines 463-466

### main.cpp GUI code to remove
- `#include "ui/app.h"` (line 12)
- `if (config.testGui || config.launchGui)` block (lines 77-93)

### CMakeLists.txt - what to remove
- imgui FetchContent_Declare block (lines 61-68)
- GLFW FetchContent_Declare block + options (lines 70-80)
- imgui_lib static library target (lines 93-108)
- src/ui/*.cpp from target_sources (lines 154-156)
- `imgui_lib` and `glfw` from dlss-compositor target_link_libraries (lines 164-169)
- `glfw` from dlss_compositor_core target_link_libraries (line 140)

### vulkan_context.cpp - what to remove (GLFW + surface/swapchain)
- `#include <GLFW/glfw3.h>` (line 9)
- glfwInit() block (lines 247-254) — GLFW init
- glfwVulkanSupported() block (lines 256-260)  
- glfwGetRequiredInstanceExtensions block (lines 291-303) — no longer needed, window extensions not needed
- `VK_KHR_SURFACE_EXTENSION_NAME` and `VK_KHR_WIN32_SURFACE_EXTENSION_NAME` additions (lines 305-306) — remove, not needed for headless
- `VK_KHR_SWAPCHAIN_EXTENSION_NAME` from device extensions (line 455) — remove, keep MAINTENANCE1
- `glfwTerminate()` block in destroy() (lines 588-591)
- `m_glfwInitialized` private member from .h (line 48)

### tests/test_cli.cpp
- NO tests for the 4 GUI flags — no changes needed here!

### Vulkan headless strategy  
- After removing GLFW, build enabledExtensions starting empty (no window surface extensions)
- Keep: VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2, VK_EXT_DEBUG_UTILS (debug), NGX required extensions
- Remove: VK_KHR_SURFACE, VK_KHR_WIN32_SURFACE, VK_KHR_SWAPCHAIN

### CMake
- CMakeLists.txt at root — needs ImGui/GLFW removed
