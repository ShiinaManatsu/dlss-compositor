#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/exr_reader.h"

enum class DlssBuffer {
    Color,
    Depth,
    MotionVectors
};

struct ChannelNames {
    std::string colorR = "RenderLayer.Combined.R";
    std::string colorG = "RenderLayer.Combined.G";
    std::string colorB = "RenderLayer.Combined.B";
    std::string colorA = "RenderLayer.Combined.A";
    std::string depth = "RenderLayer.Depth.Z";
    std::string mvX = "RenderLayer.Vector.X";
    std::string mvY = "RenderLayer.Vector.Y";
    std::string mvZ = "RenderLayer.Vector.Z";
    std::string mvW = "RenderLayer.Vector.W";
};

struct MappedBuffers {
    std::vector<float> color;
    std::vector<float> depth;
    std::vector<float> motionVectors;

    struct ChannelStatus {
        bool found = false;
        std::string usedName;
        std::string defaultNote;
    };

    std::unordered_map<std::string, ChannelStatus> status;
};

class ChannelMapper {
public:
    explicit ChannelMapper(const ChannelNames& names = ChannelNames{});

    bool mapFromExr(const ExrReader& reader, MappedBuffers& out, std::string& errorMsg) const;

private:
    ChannelNames m_names;
};
