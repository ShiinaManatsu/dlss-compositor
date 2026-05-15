#include "core/channel_mapper.h"

#include <algorithm>

ChannelMapper::ChannelMapper(const ChannelNames& names) : m_names(names) {}

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
    const bool hasAnyMotionVector = mvChannels[0] || mvChannels[1] || mvChannels[2] || mvChannels[3];
    out.status["motionVectors"] = {hasAnyMotionVector,
                                    m_names.mvX + "," + m_names.mvY + "," + m_names.mvZ + "," + m_names.mvW,
                                    hasAnyMotionVector ? std::string{} : std::string{"defaulted to zero"}};

    return true;
}
