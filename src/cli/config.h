#pragma once

#include "core/exr_writer.h"

#include <cstdint>

#include <string>
#include <vector>

enum class DlssQualityMode {
    DLAA,
    MaxQuality,
    Balanced,
    Performance,
    UltraPerformance
};

enum class TonemapMode {
    None,        // No transport encoding — pass through scene-linear (may clamp HDR)
    PQ,          // PQ (ST 2084) per-channel encode/decode (default for FG)
    Custom       // User-provided forward/inverse LUT files
};

enum class OutputPass : uint32_t {
    Beauty = 1u << 0,
    Depth = 1u << 1,
    Normals = 1u << 2,
};

inline OutputPass operator|(OutputPass a, OutputPass b) {
    return static_cast<OutputPass>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline OutputPass operator&(OutputPass a, OutputPass b) {
    return static_cast<OutputPass>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasPass(OutputPass set, OutputPass flag) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0u;
}

struct AppConfig {
    std::string inputDir;
    std::string outputDir;

    float scaleFactor = 2.0f;
    bool scaleExplicit = false;
    DlssQualityMode quality = DlssQualityMode::MaxQuality;

    int interpolateFactor = 0;
    std::string cameraDataFile;
    int memoryBudgetGB = 8;

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

    ExrCompression exrCompression = ExrCompression::Dwaa;
    float exrDwaQuality = 95.0f;
    OutputPass outputPasses = OutputPass::Beauty;

    TonemapMode tonemapMode = TonemapMode::PQ;
    bool inverseTonemapEnabled = true;
    std::string forwardLutFile;
    std::string inverseLutFile;
};
