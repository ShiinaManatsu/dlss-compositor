#include "core/exr_writer.h"

#include <tinyexr.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

struct ExrWriter::Impl {
    std::string path;
    int width = 0;
    int height = 0;
    std::vector<std::string> channelOrder;
    std::unordered_map<std::string, std::vector<float>> channelData;
};

ExrWriter::ExrWriter() : m_impl(std::make_unique<Impl>()) {}

ExrWriter::~ExrWriter() = default;

bool ExrWriter::create(const std::string& path, int width, int height, std::string& errorMsg) {
    if (width <= 0 || height <= 0) {
        errorMsg = "Invalid EXR dimensions";
        return false;
    }
    m_impl->path = path;
    m_impl->width = width;
    m_impl->height = height;
    m_impl->channelOrder.clear();
    m_impl->channelData.clear();
    errorMsg.clear();
    return true;
}

bool ExrWriter::addChannel(const std::string& name, const float* data) {
    if (!m_impl || m_impl->width <= 0 || m_impl->height <= 0 || name.empty() || data == nullptr) {
        return false;
    }
    if (m_impl->channelData.find(name) != m_impl->channelData.end()) {
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(m_impl->width) * static_cast<size_t>(m_impl->height);
    std::vector<float> channel(pixelCount);
    std::copy(data, data + pixelCount, channel.begin());
    m_impl->channelOrder.push_back(name);
    m_impl->channelData.emplace(name, std::move(channel));
    return true;
}

bool ExrWriter::write(std::string& errorMsg) {
    if (!m_impl || m_impl->path.empty() || m_impl->channelOrder.empty()) {
        errorMsg = "EXR writer is not initialized";
        return false;
    }

    std::filesystem::path outPath(m_impl->path);
    if (!outPath.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
    }

    const size_t pixelCount = static_cast<size_t>(m_impl->width) * static_cast<size_t>(m_impl->height);
    std::vector<float> rgba(pixelCount * 4u, 0.0f);

    auto getChannel = [&](const std::string& name) -> const float* {
        auto it = m_impl->channelData.find(name);
        return (it == m_impl->channelData.end()) ? nullptr : it->second.data();
    };

    const float* r = getChannel("R");
    const float* g = getChannel("G");
    const float* b = getChannel("B");
    const float* a = getChannel("A");
    if (!r) {
        r = m_impl->channelData.begin()->second.data();
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        rgba[i * 4u + 0u] = r ? r[i] : 0.0f;
        rgba[i * 4u + 1u] = g ? g[i] : 0.0f;
        rgba[i * 4u + 2u] = b ? b[i] : 0.0f;
        rgba[i * 4u + 3u] = a ? a[i] : 1.0f;
    }

    const char* err = nullptr;
    int ret = SaveEXR(rgba.data(), m_impl->width, m_impl->height, 4, 1, outPath.string().c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        errorMsg = err ? err : "Failed to write EXR";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        return false;
    }

    errorMsg.clear();
    return true;
}
