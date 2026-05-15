#include <catch2/catch_test_macros.hpp>

#include "../src/cli/config.h"
#include "../src/dlss/ngx_wrapper.h"

#include <nvsdk_ngx_defs.h>

// Numeric values from DLSS/include/nvsdk_ngx_defs.h:82-85
// J=10, K=11, L=12, M=13.
static constexpr int kPresetJ = static_cast<int>(DlssSRPreset::J);  // 10
static constexpr int kPresetK = static_cast<int>(DlssSRPreset::K);  // 11
static constexpr int kPresetL = static_cast<int>(DlssSRPreset::L);  // 12
static constexpr int kPresetM = static_cast<int>(DlssSRPreset::M);  // 13

// Sanity: enum values match the SDK header.
static_assert(kPresetJ == NVSDK_NGX_DLSS_Hint_Render_Preset_J, "J value mismatch");
static_assert(kPresetK == NVSDK_NGX_DLSS_Hint_Render_Preset_K, "K value mismatch");
static_assert(kPresetL == NVSDK_NGX_DLSS_Hint_Render_Preset_L, "L value mismatch");
static_assert(kPresetM == NVSDK_NGX_DLSS_Hint_Render_Preset_M, "M value mismatch");

// Note on the gpu_required label: this test does NOT require a real GPU;
// it exercises the AppConfig -> DlssPresetConfig -> DlssPresetSlots
// resolution chain entirely on the CPU. The label exists per plan spec
// to flag the test as part of the integration-trace bundle that CI can
// route alongside its GPU-only siblings.

TEST_CASE("IntegrationPresetTrace Auto table resolves per-quality",
          "[IntegrationPresetTrace][gpu_required]") {
    AppConfig config;
    // All preset fields default to DlssSRPreset::Auto -> NVIDIA recommended table.
    REQUIRE(config.preset == DlssSRPreset::Auto);
    REQUIRE(config.presetDLAA == DlssSRPreset::Auto);
    REQUIRE(config.presetQuality == DlssSRPreset::Auto);
    REQUIRE(config.presetBalanced == DlssSRPreset::Auto);
    REQUIRE(config.presetPerformance == DlssSRPreset::Auto);
    REQUIRE(config.presetUltraPerformance == DlssSRPreset::Auto);
    REQUIRE(config.presetUltraQuality == DlssSRPreset::Auto);

    const DlssPresetConfig presets = makeDlssPresetConfig(config);
    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    // Quality / DLAA / Balanced / UltraQuality default to K (11)
    CHECK(slots.dlaa == kPresetK);
    CHECK(slots.quality == kPresetK);
    CHECK(slots.balanced == kPresetK);
    CHECK(slots.ultraQuality == kPresetK);

    // Performance defaults to M (13)
    CHECK(slots.performance == kPresetM);

    // UltraPerformance defaults to L (12)
    CHECK(slots.ultraPerformance == kPresetL);
}

TEST_CASE("IntegrationPresetTrace global preset overrides Auto for all slots",
          "[IntegrationPresetTrace][gpu_required]") {
    AppConfig config;
    config.preset = DlssSRPreset::J;  // Global override -> J (10)
    // All per-quality fields remain Auto.

    const DlssPresetConfig presets = makeDlssPresetConfig(config);
    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    CHECK(slots.dlaa == kPresetJ);
    CHECK(slots.quality == kPresetJ);
    CHECK(slots.balanced == kPresetJ);
    CHECK(slots.performance == kPresetJ);
    CHECK(slots.ultraPerformance == kPresetJ);
    CHECK(slots.ultraQuality == kPresetJ);
}

TEST_CASE("IntegrationPresetTrace per-quality flag overrides global preset",
          "[IntegrationPresetTrace][gpu_required]") {
    AppConfig config;
    config.preset = DlssSRPreset::K;             // Global = K (11)
    config.presetPerformance = DlssSRPreset::L;  // Per-quality override: Performance = L (12)

    const DlssPresetConfig presets = makeDlssPresetConfig(config);
    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    // Performance slot picks up the per-quality override.
    CHECK(slots.performance == kPresetL);

    // All other slots fall back to the global preset.
    CHECK(slots.dlaa == kPresetK);
    CHECK(slots.quality == kPresetK);
    CHECK(slots.balanced == kPresetK);
    CHECK(slots.ultraPerformance == kPresetK);
    CHECK(slots.ultraQuality == kPresetK);
}
