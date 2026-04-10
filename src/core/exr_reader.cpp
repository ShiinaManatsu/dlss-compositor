#define TINYEXR_IMPLEMENTATION

#include "core/exr_reader.h"

#include <tinyexr.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

struct ExrReader::Impl {
    int width = 0;
    int height = 0;
    std::vector<std::string> channelOrder;
    std::unordered_map<std::string, std::vector<float>> channelData;
};

ExrReader::ExrReader() : m_impl(std::make_unique<Impl>()) {}

ExrReader::~ExrReader() = default;

void ExrReader::close() {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
        return;
    }
    m_impl->width = 0;
    m_impl->height = 0;
    m_impl->channelOrder.clear();
    m_impl->channelData.clear();
}

int ExrReader::width() const {
    return m_impl ? m_impl->width : 0;
}

int ExrReader::height() const {
    return m_impl ? m_impl->height : 0;
}

bool ExrReader::open(const std::string& path, std::string& errorMsg) {
    close();

    EXRVersion version;
    if (ParseEXRVersionFromFile(&version, path.c_str()) != TINYEXR_SUCCESS) {
        errorMsg = "Failed to parse EXR version";
        return false;
    }

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    if (ParseEXRHeaderFromFile(&header, &version, path.c_str(), &err) != TINYEXR_SUCCESS) {
        errorMsg = err ? err : "Failed to parse EXR header";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        FreeEXRHeader(&header);
        return false;
    }

    header.requested_pixel_types = reinterpret_cast<int*>(std::malloc(sizeof(int) * static_cast<size_t>(header.num_channels)));
    if (!header.requested_pixel_types) {
        errorMsg = "Failed to allocate requested pixel types";
        FreeEXRHeader(&header);
        return false;
    }
    for (int i = 0; i < header.num_channels; ++i) {
        header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
    }

    EXRImage image;
    InitEXRImage(&image);
    if (LoadEXRImageFromFile(&image, &header, path.c_str(), &err) != TINYEXR_SUCCESS) {
        errorMsg = err ? err : "Failed to load EXR image";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        FreeEXRHeader(&header);
        return false;
    }

    m_impl->width = image.width;
    m_impl->height = image.height;
    m_impl->channelOrder.reserve(header.num_channels);
    m_impl->channelData.reserve(header.num_channels);

    const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    for (int i = 0; i < header.num_channels; ++i) {
        std::string name = header.channels[i].name;
        m_impl->channelOrder.push_back(name);

        std::vector<float> data(pixelCount, 0.0f);
        if (image.images && image.images[i]) {
            const float* src = reinterpret_cast<const float*>(image.images[i]);
            std::copy(src, src + pixelCount, data.begin());
        }
        m_impl->channelData.emplace(std::move(name), std::move(data));
    }

    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    errorMsg.clear();
    return true;
}

std::vector<std::string> ExrReader::listChannels() const {
    return m_impl ? m_impl->channelOrder : std::vector<std::string>{};
}

const float* ExrReader::readChannel(const std::string& name) const {
    if (!m_impl) {
        return nullptr;
    }
    auto it = m_impl->channelData.find(name);
    if (it == m_impl->channelData.end() || it->second.empty()) {
        return nullptr;
    }
    return it->second.data();
}

std::vector<float> ExrReader::readRGBA(const std::string& r, const std::string& g,
                                       const std::string& b, const std::string& a) const {
    const int w = width();
    const int h = height();
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<float> rgba(pixelCount * 4u, 1.0f);

    const float* channels[4] = {readChannel(r), readChannel(g), readChannel(b), readChannel(a)};
    for (size_t i = 0; i < pixelCount; ++i) {
        for (int c = 0; c < 4; ++c) {
            if (channels[c]) {
                rgba[i * 4u + static_cast<size_t>(c)] = channels[c][i];
            }
        }
    }
    return rgba;
}
