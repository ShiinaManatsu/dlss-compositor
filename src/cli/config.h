#pragma once

#include <string>
#include <vector>

enum class DlssQualityMode {
    MaxQuality,
    Balanced,
    Performance,
    UltraPerformance
};

struct AppConfig {
    std::string inputDir;
    std::string outputDir;

    int scaleFactor = 2;
    DlssQualityMode quality = DlssQualityMode::Balanced;

    std::string channelMapFile;

    bool encodeVideo = false;
    std::string videoOutputFile = "output.mp4";
    int fps = 24;

    bool testNgx = false;
    bool testVulkan = false;
    bool testGui = false;
    std::string testGuiLoad;
    bool testGuiProcess = false;
    bool launchGui = false;
    bool showHelp = false;
    bool showVersion = false;
};
