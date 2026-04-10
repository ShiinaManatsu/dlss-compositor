#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/exr_reader.h"

enum class DlssBuffer {
    Color,
    Depth,
    MotionVectors,
    DiffuseAlbedo,
    SpecularAlbedo,
    Normals,
    Roughness
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
    std::string diffR = "RenderLayer.DiffCol.R";
    std::string diffG = "RenderLayer.DiffCol.G";
    std::string diffB = "RenderLayer.DiffCol.B";
    std::string specR = "RenderLayer.GlossCol.R";
    std::string specG = "RenderLayer.GlossCol.G";
    std::string specB = "RenderLayer.GlossCol.B";
    std::string normalX = "RenderLayer.Normal.X";
    std::string normalY = "RenderLayer.Normal.Y";
    std::string normalZ = "RenderLayer.Normal.Z";
    std::string roughness = "RenderLayer.Roughness.X";
};

struct MappedBuffers {
    std::vector<float> color;
    std::vector<float> depth;
    std::vector<float> motionVectors;
    std::vector<float> diffuseAlbedo;
    std::vector<float> specularAlbedo;
    std::vector<float> normals;
    std::vector<float> roughness;

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

    std::vector<float> extractOrDefault(const ExrReader& reader, const std::string& channelName,
                                        float defaultVal, int width, int height,
                                        MappedBuffers::ChannelStatus& status) const;
};
