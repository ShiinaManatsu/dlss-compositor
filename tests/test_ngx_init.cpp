#include <catch2/catch_test_macros.hpp>

#include "../src/dlss/ngx_wrapper.h"
#include "../src/gpu/vulkan_context.h"

// GPU tests require an NVIDIA RTX GPU — tag [gpu] so CI can skip.
// Run locally: ctest -C Release --label-regex "gpu"

TEST_CASE("NgxContext - init and capability query on RTX GPU", "[gpu]") {
    dlss::VulkanContext vkCtx;

    // SR and FG should be available on RTX 40+.
    dlss::NgxContext ngx(
        vkCtx.instance(),
        vkCtx.physicalDevice(),
        vkCtx.device());

    REQUIRE(ngx.isSrAvailable());
    REQUIRE(ngx.isFgAvailable());
    REQUIRE(ngx.getFgMaxMultiplier() >= 2);

    auto versions = ngx.getDllVersions();
    INFO("DLSS SR DLL version: " << versions.sr);
    INFO("DLSS FG DLL version: " << versions.fg);
    REQUIRE(!versions.sr.empty());
    REQUIRE(!versions.fg.empty());
    // Should contain version number from the loaded NVIDIA NGX runtime.
    REQUIRE(versions.sr != "unknown");
}

TEST_CASE("NgxContext - multiple init/destroy cycles", "[gpu]") {
    // Verify NGX can be initialized, destroyed, and re-initialized.
    for (int i = 0; i < 3; ++i) {
        dlss::VulkanContext vkCtx;
        dlss::NgxContext ngx(
            vkCtx.instance(), vkCtx.physicalDevice(), vkCtx.device());
        REQUIRE(ngx.isSrAvailable());
    }
    SUCCEED("NGX init/destroy cycle x3 passed");
}

TEST_CASE("NgxContext - NGX params not null after init", "[gpu]") {
    dlss::VulkanContext vkCtx;
    dlss::NgxContext ngx(
        vkCtx.instance(), vkCtx.physicalDevice(), vkCtx.device());
    REQUIRE(ngx.params() != nullptr);
}
