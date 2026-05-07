#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../src/cli/cli_parser.h"
#include "../src/cli/config.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct FakeArgs {
    std::vector<const char*> args;
    explicit FakeArgs(std::initializer_list<const char*> a) : args(a) {
        args.insert(args.begin(), "dlss-compositor");
    }
    int argc() const { return static_cast<int>(args.size()); }
    char** argv() const { return const_cast<char**>(args.data()); }
};

TEST_CASE("CliParser - --help flag", "[cli]") {
    FakeArgs fa{"--help"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.showHelp);
}

TEST_CASE("CliParser - --version flag", "[cli]") {
    FakeArgs fa{"--version"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.showVersion);
}

TEST_CASE("CliParser - input/output dirs", "[cli]") {
    FakeArgs fa{"--input-dir", "./frames/", "--output-dir", "./out/"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.inputDir == "./frames/");
    REQUIRE(cfg.outputDir == "./out/");
}

TEST_CASE("CliParser - scale and quality", "[cli]") {
    FakeArgs fa{"--scale", "4", "--quality", "Performance"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.scaleFactor == 4);
    REQUIRE(cfg.quality == DlssQualityMode::Performance);
}

TEST_CASE("CliParser - float scale values", "[cli]") {
    {
        FakeArgs fa{"--scale", "1.5"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(ok);
        REQUIRE(cfg.scaleFactor == Catch::Approx(1.5f));
        REQUIRE(cfg.scaleExplicit);
    }

    {
        FakeArgs fa{"--scale", "1.0"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(ok);
        REQUIRE(cfg.scaleFactor == Catch::Approx(1.0f));
        REQUIRE(cfg.scaleExplicit);
    }

    {
        FakeArgs fa{"--scale", "2.0"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(ok);
        REQUIRE(cfg.scaleFactor == Catch::Approx(2.0f));
        REQUIRE(cfg.scaleExplicit);
    }

    {
        FakeArgs fa{"--scale", "2"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(ok);
        REQUIRE(cfg.scaleFactor == Catch::Approx(2.0f));
        REQUIRE(cfg.scaleExplicit);
    }
}

TEST_CASE("CliParser - invalid float scale values", "[cli]") {
    {
        FakeArgs fa{"--scale", "0.5"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(!ok);
        REQUIRE(!err.empty());
    }

    {
        FakeArgs fa{"--scale", "9.0"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(!ok);
        REQUIRE(!err.empty());
    }

    {
        FakeArgs fa{"--scale", "abc"};
        AppConfig cfg;
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
        REQUIRE(!ok);
        REQUIRE(!err.empty());
    }
}

TEST_CASE("CliParser - DLAA quality mode", "[cli]") {
    FakeArgs fa{"--quality", "DLAA"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.quality == DlssQualityMode::DLAA);
}

TEST_CASE("CliParser - invalid DLAA quality casing", "[cli]") {
    FakeArgs fa{"--quality", "dlaa"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - invalid scale", "[cli]") {
    FakeArgs fa{"--scale", "9"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - invalid quality mode", "[cli]") {
    FakeArgs fa{"--quality", "BadMode"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - encode-video default filename", "[cli]") {
    FakeArgs fa{"--encode-video"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.encodeVideo);
    REQUIRE(cfg.videoOutputFile == "output.mp4");
}

TEST_CASE("CliParser - encode-video custom filename", "[cli]") {
    FakeArgs fa{"--encode-video", "myvideo.mp4"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.encodeVideo);
    REQUIRE(cfg.videoOutputFile == "myvideo.mp4");
}

TEST_CASE("CliParser - test-ngx flag", "[cli]") {
    FakeArgs fa{"--test-ngx"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.testNgx);
}

TEST_CASE("CliParser - fps flag", "[cli]") {
    FakeArgs fa{"--fps", "30"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.fps == 30);
}

TEST_CASE("CliParser - --interpolate 2x", "[cli]") {
    FakeArgs fa{"--interpolate", "2x", "--camera-data", "camera.json"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.interpolateFactor == 2);
    REQUIRE(cfg.cameraDataFile == "camera.json");
}

TEST_CASE("CliParser - --interpolate 4x", "[cli]") {
    FakeArgs fa{"--interpolate", "4x", "--camera-data", "camera.json"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.interpolateFactor == 4);
}

TEST_CASE("CliParser - --interpolate invalid value", "[cli]") {
    FakeArgs fa{"--interpolate", "3x", "--camera-data", "camera.json"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - --interpolate without --camera-data", "[cli]") {
    FakeArgs fa{"--interpolate", "2x"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - combined --scale and --interpolate", "[cli]") {
    FakeArgs fa{"--scale", "2", "--interpolate", "2x", "--camera-data", "camera.json"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.scaleFactor == 2);
    REQUIRE(cfg.interpolateFactor == 2);
    REQUIRE(cfg.cameraDataFile == "camera.json");
}

TEST_CASE("CliParser - --memory-budget valid", "[cli]") {
    FakeArgs fa{"--memory-budget", "4"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.memoryBudgetGB == 4);
}

TEST_CASE("CliParser - --memory-budget invalid", "[cli]") {
    FakeArgs fa{"--memory-budget", "0"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - --memory-budget missing value", "[cli]") {
    FakeArgs fa{"--memory-budget"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - --tonemap custom with LUT files", "[cli]") {
    FakeArgs fa{"--tonemap", "custom", "--tonemap-lut", "fwd.bin", "--inverse-tonemap-lut", "inv.bin"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.tonemapMode == TonemapMode::Custom);
}

TEST_CASE("CliParser - --memory-budget default", "[cli]") {
    FakeArgs fa{"--input-dir", "test/"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.memoryBudgetGB == 8);
}

TEST_CASE("CliParser - --tonemap custom without lut files fails", "[cli]") {
    FakeArgs fa{"--tonemap", "custom"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(!ok);
    REQUIRE(!err.empty());
}

TEST_CASE("CliParser - --source-fps valid", "[cli]") {
    FakeArgs fa{"--source-fps", "24"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.sourceFps == 24);
}

TEST_CASE("CliParser - --source-fps default", "[cli]") {
    FakeArgs fa{"--input-dir", "test/"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.sourceFps == 30);
}

TEST_CASE("CliParser - --source-fps zero fallback", "[cli]") {
    FakeArgs fa{"--source-fps", "0"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.sourceFps == 30);  // fallback to default
}

TEST_CASE("CliParser - --preset auto", "[cli]") {
    FakeArgs fa{"--preset", "auto"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.preset == DlssSRPreset::Auto);
}

TEST_CASE("CliParser - --preset Auto (capitalized)", "[cli]") {
    FakeArgs fa{"--preset", "Auto"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.preset == DlssSRPreset::Auto);
}

TEST_CASE("CliParser - default preset is Auto", "[cli]") {
    FakeArgs fa{"--input-dir", "test/"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.preset == DlssSRPreset::Auto);
}

TEST_CASE("CliParser - --preset K vs Auto give different values", "[cli]") {
    AppConfig cfgAuto;
    {
        FakeArgs fa{"--preset", "auto"};
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfgAuto, err);
        REQUIRE(ok);
    }

    AppConfig cfgK;
    {
        FakeArgs fa{"--preset", "K"};
        std::string err;
        bool ok = CliParser::parse(fa.argc(), fa.argv(), cfgK, err);
        REQUIRE(ok);
    }

    REQUIRE(cfgAuto.preset == DlssSRPreset::Auto);
    REQUIRE(cfgK.preset == DlssSRPreset::K);
    // Auto and K are different enum values
    REQUIRE(static_cast<int>(cfgAuto.preset) != static_cast<int>(cfgK.preset));
}

namespace {
    // Helper: create a temporary directory with a unique name
    std::string createTempDir(const char* label) {
        static int counter = 0;
        const std::string path = (std::filesystem::temp_directory_path() /
            (std::string("dlss_") + label + "_" + std::to_string(++counter))).string();
        std::filesystem::create_directories(path);
        return path;
    }

    // Helper: create a small file at the given path
    void touchFile(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (f) {
            std::fprintf(f, "{}\n");
            std::fclose(f);
        }
    }
}

TEST_CASE("CliParser - auto-discover camera.json from input dir", "[cli]") {
    const std::string tmpDir = createTempDir("camera_autodiscover");
    touchFile((std::filesystem::path(tmpDir) / "camera.json").string());

    std::string inputDirArg = tmpDir;
    FakeArgs fa{"--input-dir", inputDirArg.c_str(), "--interpolate", "2x"};
    AppConfig cfg;
    std::string err;

    const bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);

    REQUIRE(ok);
    REQUIRE(cfg.cameraDataFile == (std::filesystem::path(tmpDir) / "camera.json").string());

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("CliParser - no auto-discover when camera.json missing", "[cli]") {
    const std::string tmpDir = createTempDir("camera_missing");

    std::string inputDirArg = tmpDir;
    FakeArgs fa{"--input-dir", inputDirArg.c_str()};
    AppConfig cfg;
    std::string err;

    const bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);

    REQUIRE(ok);
    REQUIRE(cfg.cameraDataFile.empty());

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("CliParser - explicit --camera-data wins over auto-discover", "[cli]") {
    const std::string tmpDir = createTempDir("camera_explicit_wins");
    touchFile((std::filesystem::path(tmpDir) / "camera.json").string());

    std::string inputDirArg = tmpDir;
    FakeArgs fa{"--input-dir", inputDirArg.c_str(), "--camera-data", "explicit.json", "--interpolate", "2x"};
    AppConfig cfg;
    std::string err;

    const bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);

    REQUIRE(ok);
    REQUIRE(cfg.cameraDataFile == "explicit.json");  // explicit flag, not auto-discovered

    std::filesystem::remove_all(tmpDir);
}
