#include <catch2/catch_test_macros.hpp>

#include "../src/cli/cli_parser.h"
#include "../src/cli/config.h"

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

bool parseOk(std::initializer_list<const char*> values, AppConfig& outConfig) {
    FakeArgs args(values);
    std::string err;
    return CliParser::parse(args.argc(), args.argv(), outConfig, err);
}

bool parseFails(std::initializer_list<const char*> values, std::string& errorOut) {
    FakeArgs args(values);
    AppConfig config;
    errorOut.clear();
    const bool ok = CliParser::parse(args.argc(), args.argv(), config, errorOut);
    return !ok && !errorOut.empty();
}

} // namespace

TEST_CASE("CliAdvancedFlags defaults preserve prior hardcoded behavior", "[cli][unit][CliAdvancedFlags]") {
    AppConfig config;  // no flags parsed
    CHECK(config.featureFlagHDR == true);
    CHECK(config.featureFlagMVLowRes == true);
    CHECK(config.featureFlagAutoExposure == true);
    CHECK(config.enableOutputSubrects == false);
    CHECK(config.exposureScale == 1.0f);
    CHECK(config.frameTimeDeltaMsOverride == -1.0f);
    CHECK(config.dlssSharpness == 0.0f);
    CHECK(config.enableDynamicResolution == false);
}

TEST_CASE("CliAdvancedFlags HDR boolean pair", "[cli][unit][CliAdvancedFlags]") {
    SECTION("--feature-flag-hdr sets to true") {
        AppConfig config;
        config.featureFlagHDR = false;
        REQUIRE(parseOk({"--feature-flag-hdr"}, config));
        CHECK(config.featureFlagHDR == true);
    }
    SECTION("--no-feature-flag-hdr sets to false") {
        AppConfig config;
        REQUIRE(parseOk({"--no-feature-flag-hdr"}, config));
        CHECK(config.featureFlagHDR == false);
    }
}

TEST_CASE("CliAdvancedFlags MV-low-res boolean pair", "[cli][unit][CliAdvancedFlags]") {
    SECTION("--feature-flag-mv-low-res") {
        AppConfig config;
        config.featureFlagMVLowRes = false;
        REQUIRE(parseOk({"--feature-flag-mv-low-res"}, config));
        CHECK(config.featureFlagMVLowRes == true);
    }
    SECTION("--no-feature-flag-mv-low-res") {
        AppConfig config;
        REQUIRE(parseOk({"--no-feature-flag-mv-low-res"}, config));
        CHECK(config.featureFlagMVLowRes == false);
    }
}

TEST_CASE("CliAdvancedFlags auto-exposure boolean pair", "[cli][unit][CliAdvancedFlags]") {
    SECTION("--feature-flag-auto-exposure") {
        AppConfig config;
        config.featureFlagAutoExposure = false;
        REQUIRE(parseOk({"--feature-flag-auto-exposure"}, config));
        CHECK(config.featureFlagAutoExposure == true);
    }
    SECTION("--no-feature-flag-auto-exposure") {
        AppConfig config;
        REQUIRE(parseOk({"--no-feature-flag-auto-exposure"}, config));
        CHECK(config.featureFlagAutoExposure == false);
    }
}

TEST_CASE("CliAdvancedFlags enable-output-subrects boolean pair", "[cli][unit][CliAdvancedFlags]") {
    SECTION("--enable-output-subrects sets true") {
        AppConfig config;
        REQUIRE(parseOk({"--enable-output-subrects"}, config));
        CHECK(config.enableOutputSubrects == true);
    }
    SECTION("--no-enable-output-subrects sets false") {
        AppConfig config;
        config.enableOutputSubrects = true;
        REQUIRE(parseOk({"--no-enable-output-subrects"}, config));
        CHECK(config.enableOutputSubrects == false);
    }
}

TEST_CASE("CliAdvancedFlags exposure-scale validates range", "[cli][unit][CliAdvancedFlags]") {
    SECTION("valid mid-range value accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--exposure-scale", "1.5"}, config));
        CHECK(config.exposureScale == 1.5f);
    }
    SECTION("upper-bound 100.0 accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--exposure-scale", "100"}, config));
        CHECK(config.exposureScale == 100.0f);
    }
    SECTION("zero rejected") {
        std::string err;
        CHECK(parseFails({"--exposure-scale", "0"}, err));
    }
    SECTION("negative rejected") {
        std::string err;
        CHECK(parseFails({"--exposure-scale", "-1"}, err));
    }
    SECTION("> 100 rejected") {
        std::string err;
        CHECK(parseFails({"--exposure-scale", "101"}, err));
    }
    SECTION("non-numeric rejected") {
        std::string err;
        CHECK(parseFails({"--exposure-scale", "abc"}, err));
    }
    SECTION("missing value rejected") {
        std::string err;
        CHECK(parseFails({"--exposure-scale"}, err));
    }
}

TEST_CASE("CliAdvancedFlags frame-time-delta-ms validates positive", "[cli][unit][CliAdvancedFlags]") {
    SECTION("valid positive value accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--frame-time-delta-ms", "16.667"}, config));
        CHECK(config.frameTimeDeltaMsOverride == 16.667f);
    }
    SECTION("zero rejected") {
        std::string err;
        CHECK(parseFails({"--frame-time-delta-ms", "0"}, err));
    }
    SECTION("negative rejected") {
        std::string err;
        CHECK(parseFails({"--frame-time-delta-ms", "-5"}, err));
    }
    SECTION("missing value rejected") {
        std::string err;
        CHECK(parseFails({"--frame-time-delta-ms"}, err));
    }
}

TEST_CASE("CliAdvancedFlags dlss-sharpness validates unit range", "[cli][unit][CliAdvancedFlags]") {
    SECTION("0.0 accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--dlss-sharpness", "0.0"}, config));
        CHECK(config.dlssSharpness == 0.0f);
    }
    SECTION("0.5 accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--dlss-sharpness", "0.5"}, config));
        CHECK(config.dlssSharpness == 0.5f);
    }
    SECTION("1.0 accepted") {
        AppConfig config;
        REQUIRE(parseOk({"--dlss-sharpness", "1.0"}, config));
        CHECK(config.dlssSharpness == 1.0f);
    }
    SECTION("1.5 rejected (above max)") {
        std::string err;
        CHECK(parseFails({"--dlss-sharpness", "1.5"}, err));
    }
    SECTION("-0.1 rejected (below min)") {
        std::string err;
        CHECK(parseFails({"--dlss-sharpness", "-0.1"}, err));
    }
    SECTION("missing value rejected") {
        std::string err;
        CHECK(parseFails({"--dlss-sharpness"}, err));
    }
}

TEST_CASE("CliAdvancedFlags enable-dynamic-resolution boolean pair", "[cli][unit][CliAdvancedFlags]") {
    SECTION("--enable-dynamic-resolution sets true") {
        AppConfig config;
        REQUIRE(parseOk({"--enable-dynamic-resolution"}, config));
        CHECK(config.enableDynamicResolution == true);
    }
    SECTION("--no-enable-dynamic-resolution sets false") {
        AppConfig config;
        config.enableDynamicResolution = true;
        REQUIRE(parseOk({"--no-enable-dynamic-resolution"}, config));
        CHECK(config.enableDynamicResolution == false);
    }
}

TEST_CASE("CliAdvancedFlags combined flags coexist without interference", "[cli][unit][CliAdvancedFlags]") {
    AppConfig config;
    REQUIRE(parseOk({
        "--no-feature-flag-hdr",
        "--no-feature-flag-mv-low-res",
        "--no-feature-flag-auto-exposure",
        "--enable-output-subrects",
        "--exposure-scale", "2.5",
        "--frame-time-delta-ms", "33.333",
        "--dlss-sharpness", "0.4",
        "--enable-dynamic-resolution"
    }, config));
    CHECK(config.featureFlagHDR == false);
    CHECK(config.featureFlagMVLowRes == false);
    CHECK(config.featureFlagAutoExposure == false);
    CHECK(config.enableOutputSubrects == true);
    CHECK(config.exposureScale == 2.5f);
    CHECK(config.frameTimeDeltaMsOverride == 33.333f);
    CHECK(config.dlssSharpness == 0.4f);
    CHECK(config.enableDynamicResolution == true);
}

