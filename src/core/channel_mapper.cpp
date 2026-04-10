#include "core/channel_mapper.h"

#include <algorithm>

ChannelMapper::ChannelMapper(const ChannelNames& names) : m_names(names) {}

std::vector<float> ChannelMapper::extractOrDefault(const ExrReader& reader, const std::string& channelName,
                                                   float defaultVal, int width, int height,
                                                   MappedBuffers::ChannelStatus& status) const {
    const size_t pixelCount = static_cast<size_t>(std::max(0, width)) * static_cast<size_t>(std::max(0, height));
    std::vector<float> values(pixelCount, defaultVal);
    const float* src = reader.readChannel(channelName);
    if (src) {
        status.found = true;
        status.usedName = channelName;
        status.defaultNote.clear();
        std::copy(src, src + pixelCount, values.begin());
    } else {
        status.found = false;
        status.usedName = channelName;
        status.defaultNote = "defaulted";
    }
    return values;
}

bool ChannelMapper::mapFromExr(const ExrReader& reader, MappedBuffers& out, std::string& errorMsg) const {
    errorMsg.clear();
    out.status.clear();

    const int width = reader.width();
    const int height = reader.height();
    if (width <= 0 || height <= 0) {
        errorMsg = "EXR reader is empty";
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    const float* colorChannels[4] = {
        reader.readChannel(m_names.colorR),
        reader.readChannel(m_names.colorG),
        reader.readChannel(m_names.colorB),
        reader.readChannel(m_names.colorA),
    };
    const float* depthChannel = reader.readChannel(m_names.depth);
    if (!colorChannels[0] || !colorChannels[1] || !colorChannels[2] || !colorChannels[3]) {
        errorMsg = "Missing required color channel(s)";
        return false;
    }
    if (!depthChannel) {
        errorMsg = "Missing required channel: " + m_names.depth;
        return false;
    }
    out.color = reader.readRGBA(m_names.colorR, m_names.colorG, m_names.colorB, m_names.colorA);
    out.depth.assign(depthChannel, depthChannel + pixelCount);
    out.status["color"] = {true, m_names.colorR + "," + m_names.colorG + "," + m_names.colorB + "," + m_names.colorA, {}};
    out.status["depth"] = {true, m_names.depth, {}};

    out.motionVectors.resize(pixelCount * 4u, 0.0f);
    const float* mvChannels[4] = {
        reader.readChannel(m_names.mvX),
        reader.readChannel(m_names.mvY),
        reader.readChannel(m_names.mvZ),
        reader.readChannel(m_names.mvW),
    };
    for (int c = 0; c < 4; ++c) {
        if (mvChannels[c]) {
            for (size_t i = 0; i < pixelCount; ++i) {
                out.motionVectors[i * 4u + static_cast<size_t>(c)] = mvChannels[c][i];
            }
        }
    }
    if (!mvChannels[0] || !mvChannels[1] || !mvChannels[2] || !mvChannels[3]) {
        errorMsg = "Missing required motion vector channel(s)";
        return false;
    }
    out.status["motionVectors"] = {mvChannels[0] || mvChannels[1] || mvChannels[2] || mvChannels[3],
                                    m_names.mvX + "," + m_names.mvY + "," + m_names.mvZ + "," + m_names.mvW,
                                    (mvChannels[0] || mvChannels[1] || mvChannels[2] || mvChannels[3]) ? std::string{} : std::string{"defaulted to zero"}};

    out.diffuseAlbedo.resize(pixelCount * 3u, 1.0f);
    out.specularAlbedo.resize(pixelCount * 3u, 0.0f);
    out.normals.resize(pixelCount * 3u, 0.0f);
    out.roughness = extractOrDefault(reader, m_names.roughness, 0.5f, width, height, out.status["roughness"]);

    const float* diffChannels[3] = {reader.readChannel(m_names.diffR), reader.readChannel(m_names.diffG), reader.readChannel(m_names.diffB)};
    const float* specChannels[3] = {reader.readChannel(m_names.specR), reader.readChannel(m_names.specG), reader.readChannel(m_names.specB)};
    const float* normalChannels[3] = {reader.readChannel(m_names.normalX), reader.readChannel(m_names.normalY), reader.readChannel(m_names.normalZ)};

    const struct {
        const char* key;
        const float* const* channels;
        std::vector<float>* dst;
        float defaults[3];
    } rgbSets[] = {
        {"diffuseAlbedo", diffChannels, &out.diffuseAlbedo, {1.0f, 1.0f, 1.0f}},
        {"specularAlbedo", specChannels, &out.specularAlbedo, {0.0f, 0.0f, 0.0f}},
        {"normals", normalChannels, &out.normals, {0.0f, 0.0f, 1.0f}},
    };

    for (const auto& set : rgbSets) {
        for (size_t i = 0; i < pixelCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float* channel = set.channels[c];
                (*set.dst)[i * 3u + static_cast<size_t>(c)] = channel ? channel[i] : set.defaults[c];
            }
        }
    }

    out.status["diffuseAlbedo"] = {diffChannels[0] || diffChannels[1] || diffChannels[2], m_names.diffR + "," + m_names.diffG + "," + m_names.diffB,
                                   (diffChannels[0] || diffChannels[1] || diffChannels[2]) ? std::string{} : std::string{"defaulted to white"}};
    out.status["specularAlbedo"] = {specChannels[0] || specChannels[1] || specChannels[2], m_names.specR + "," + m_names.specG + "," + m_names.specB,
                                    (specChannels[0] || specChannels[1] || specChannels[2]) ? std::string{} : std::string{"defaulted to black"}};
    out.status["normals"] = {normalChannels[0] || normalChannels[1] || normalChannels[2], m_names.normalX + "," + m_names.normalY + "," + m_names.normalZ,
                              (normalChannels[0] || normalChannels[1] || normalChannels[2]) ? std::string{} : std::string{"defaulted to +Z"}};
    out.status["roughness"].usedName = m_names.roughness;

    return true;
}
