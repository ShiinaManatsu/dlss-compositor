#include <catch2/catch_test_macros.hpp>

#include "../src/dlss/ngx_wrapper.h"

#include <nvsdk_ngx_defs.h>

#include <cstdint>
#include <string>

// These tests do NOT require a GPU. They verify:
//   1. The NGX API version constants used by NgxContext::init() match the SDK
//      header (and therefore the fallback value is exactly primary - 1).
//   2. NgxContext starts in a clean "not yet initialized" state.
//   3. dlssDllVersion() never crashes and always returns a non-empty string,
//      whether or not nvngx_dlss.dll is currently mapped into the process.

TEST_CASE("NgxBanner API version constant matches SDK header", "[NgxBanner][unit]") {
    // Sanity-check the bundled SDK header value. If NVIDIA bumps the version
    // in a future submodule update, the fallback math (primary - 1) and the
    // ngx_api_version_used NDJSON field will need to be re-validated.
    CHECK(NVSDK_NGX_VERSION_API_MACRO == 0x15u);
    CHECK(static_cast<uint32_t>(NVSDK_NGX_Version_API) == 0x15u);
}

TEST_CASE("NgxBanner uninitialized context exposes zero-state version info", "[NgxBanner][unit]") {
    NgxContext ctx;
    CHECK(ctx.isInitialized() == false);
    // Before init() runs, the API version is unset (0) and fallback is false.
    CHECK(ctx.ngxApiVersionUsed() == 0u);
    CHECK(ctx.ngxUsedFallback() == false);
}

TEST_CASE("NgxBanner DLL version returns a non-empty string without crashing",
          "[NgxBanner][unit]") {
    NgxContext ctx;
    // The function uses GetModuleHandleW so it never throws/crashes — it
    // either returns a real version string ("310.5.3.0") or a sentinel
    // ("not loaded" / "path unknown" / "version unavailable"). All sentinels
    // are non-empty by construction.
    const std::string ver = ctx.dlssDllVersion();
    CHECK_FALSE(ver.empty());
}
