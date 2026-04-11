# Decisions — dlss-frame-generation

## 2026-04-11 — Initial Session

### Architecture
- Standalone FG mode: no RR→FG pipeline chaining
- DlssFgProcessor mirrors DlssRRProcessor pattern
- CameraDataLoader is independent of NgxContext
- m_fgFeatureHandle added alongside existing m_featureHandle in NgxContext
- Option A: minimal disruption to NgxContext

### JSON Parsing
- Check for nlohmann/json first; if not in CMake, add via FetchContent
- Camera.json format: "frames": { "0001": { matrix_world, projection, fov, ... } }
- clipToPrevClip computed at RUNTIME, NOT pre-stored in JSON

### Output Naming
- 2x: original_0001→0001, interp→0002, original_0002→0003, ...
- 4x: original_0001→0001, interp1→0002, interp2→0003, interp3→0004, original_0002→0005, ...

### Hardware
- RTX 40+: 2x FG (multiFrameCount=1)
- RTX 50+: 4x MFG (multiFrameCount=3)
- If 4x requested but maxMultiFrameCount < 3: clear error message

### NativeBackbufferFormat Investigation
- Task 2 must test VK_FORMAT_R16G16B16A16_SFLOAT (=97) vs DXGI_FORMAT_R16G16B16A16_FLOAT (=10)
- Try VkFormat first since Vulkan API path

### Build Integration
- Keep DLSS-G wiring parallel to DLSS-RR by copying `nvngx_dlssg.dll` in the main target and test target post-build steps.
- Add stub camera/FG translation units immediately so CMake source lists remain valid before their real logic is implemented.
