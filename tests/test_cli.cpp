#include <catch2/catch_test_macros.hpp>

#include "../src/cli/cli_parser.h"
#include "../src/cli/config.h"

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

TEST_CASE("CliParser - invalid scale", "[cli]") {
    FakeArgs fa{"--scale", "7"};
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

TEST_CASE("CliParser - --memory-budget default", "[cli]") {
    FakeArgs fa{"--input-dir", "test/"};
    AppConfig cfg;
    std::string err;
    bool ok = CliParser::parse(fa.argc(), fa.argv(), cfg, err);
    REQUIRE(ok);
    REQUIRE(cfg.memoryBudgetGB == 8);
}
