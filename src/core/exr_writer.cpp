#include "core/exr_writer.h"

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFloatAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

struct ExrWriter::Impl {
    std::string path;
    int width = 0;
    int height = 0;
    ExrCompression compression = ExrCompression::Dwaa;
    float dwaQuality = 95.0f;
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

void ExrWriter::setCompression(ExrCompression compression, float dwaQuality) {
    if (!m_impl) {
        return;
    }
    m_impl->compression = compression;
    m_impl->dwaQuality = dwaQuality;
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

    try {
        Imf::Header header(m_impl->width, m_impl->height);
        switch (m_impl->compression) {
        case ExrCompression::None: header.compression() = Imf::NO_COMPRESSION; break;
        case ExrCompression::Zip: header.compression() = Imf::ZIP_COMPRESSION; break;
        case ExrCompression::Zips: header.compression() = Imf::ZIPS_COMPRESSION; break;
        case ExrCompression::Piz: header.compression() = Imf::PIZ_COMPRESSION; break;
        case ExrCompression::Dwaa:
            header.compression() = Imf::DWAA_COMPRESSION;
            header.insert("dwaCompressionLevel", Imf::FloatAttribute(m_impl->dwaQuality));
            break;
        case ExrCompression::Dwab:
            header.compression() = Imf::DWAB_COMPRESSION;
            header.insert("dwaCompressionLevel", Imf::FloatAttribute(m_impl->dwaQuality));
            break;
        }
        Imf::FrameBuffer frameBuffer;

        for (const std::string& name : m_impl->channelOrder) {
            auto it = m_impl->channelData.find(name);
            if (it == m_impl->channelData.end()) {
                errorMsg = "Missing channel data for: " + name;
                return false;
            }

            header.channels().insert(name.c_str(), Imf::Channel(Imf::FLOAT));
            frameBuffer.insert(name.c_str(),
                               Imf::Slice(Imf::FLOAT,
                                          reinterpret_cast<char*>(it->second.data()),
                                          sizeof(float),
                                          sizeof(float) * static_cast<size_t>(m_impl->width)));
        }

        Imf::OutputFile outputFile(outPath.string().c_str(), header);
        outputFile.setFrameBuffer(frameBuffer);
        outputFile.writePixels(m_impl->height);
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }

    errorMsg.clear();
    return true;
}
