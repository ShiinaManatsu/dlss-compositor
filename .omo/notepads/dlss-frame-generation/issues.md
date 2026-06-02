# Issues — dlss-frame-generation

## 2026-04-11 — Initial Session

### Open Questions / Risks
1. **NativeBackbufferFormat**: VkFormat (97) vs DXGI_FORMAT (10)? Must be validated in Task 2 — this is a FEASIBILITY GATE
2. **Headless DLSS-G**: dlssg-to-fsr3 project (4.9k stars) confirms raw NGX works without swapchain, but need to validate
3. **Depth convention**: Blender Z pass = linear distance (not NDC). depthInverted=false should be correct but validate
4. **VRAM**: Keeping previous frame textures alive across loop iterations — ensure proper lifecycle management in Task 7

### Pre-existing LSP Errors (NOT NEW)
- exr_reader.cpp: OpenEXR headers not on LSP include path (build-time only)
- texture_pipeline.cpp: VMA headers not on LSP path
- These are known issues unrelated to this plan
