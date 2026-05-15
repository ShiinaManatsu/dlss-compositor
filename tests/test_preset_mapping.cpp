#include <catch2/catch_test_macros.hpp>

#include "../src/cli/cli_parser.h"
#include "../src/cli/config.h"
#include "../src/dlss/ngx_wrapper.h"

#include <string>
#include <vector>

namespace {

struct FakeArgs {
    std::vector<const char*> args;

    explicit FakeArgs(std::initializer_list<const char*> values) : args(values) {
        args.insert(args.begin(), "dlss-compositor");
    }

    int argc() const {
        return static_cast<int>(args.size());
    }

    char** argv() const {
        return const_cast<char**>(args.data());
    }
};

void checkAllPresetSlots(const DlssPresetSlots& slots, int expected) {
    CHECK(slots.dlaa == expected);
    CHECK(slots.quality == expected);
    CHECK(slots.balanced == expected);
    CHECK(slots.performance == expected);
    CHECK(slots.ultraPerformance == expected);
    CHECK(slots.ultraQuality == expected);
}

} // namespace

TEST_CASE("PresetMapping resolves Auto to NVIDIA recommended presets", "[PresetMapping][unit]") {
    const DlssPresetConfig presets;
    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    CHECK(slots.dlaa == NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    CHECK(slots.quality == NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    CHECK(slots.balanced == NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    CHECK(slots.performance == NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    CHECK(slots.ultraPerformance == NVSDK_NGX_DLSS_Hint_Render_Preset_L);
    CHECK(slots.ultraQuality == NVSDK_NGX_DLSS_Hint_Render_Preset_K);
}

TEST_CASE("PresetMapping uses global override when per-quality preset is Auto", "[PresetMapping][unit]") {
    DlssPresetConfig presets;
    presets.globalPreset = DlssSRPreset::K;
    presets.performance = DlssSRPreset::Auto;

    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    CHECK(slots.performance == NVSDK_NGX_DLSS_Hint_Render_Preset_K);
}

TEST_CASE("PresetMapping uses per-quality override before global preset", "[PresetMapping][unit]") {
    DlssPresetConfig presets;
    presets.globalPreset = DlssSRPreset::K;
    presets.performance = DlssSRPreset::L;

    const DlssPresetSlots slots = resolveDlssPresetSlots(presets);

    CHECK(slots.performance == NVSDK_NGX_DLSS_Hint_Render_Preset_L);
}

TEST_CASE("PresetMapping preserves legacy --preset K behavior", "[PresetMapping][unit]") {
    FakeArgs args{"--preset", "K"};
    AppConfig config;
    std::string errorMsg;

    const bool ok = CliParser::parse(args.argc(), args.argv(), config, errorMsg);
    REQUIRE(ok);
    REQUIRE(config.preset == DlssSRPreset::K);
    REQUIRE(config.presetDLAA == DlssSRPreset::Auto);
    REQUIRE(config.presetQuality == DlssSRPreset::Auto);
    REQUIRE(config.presetBalanced == DlssSRPreset::Auto);
    REQUIRE(config.presetPerformance == DlssSRPreset::Auto);
    REQUIRE(config.presetUltraPerformance == DlssSRPreset::Auto);
    REQUIRE(config.presetUltraQuality == DlssSRPreset::Auto);

    checkAllPresetSlots(resolveDlssPresetSlots(makeDlssPresetConfig(config)),
                        NVSDK_NGX_DLSS_Hint_Render_Preset_K);
}
