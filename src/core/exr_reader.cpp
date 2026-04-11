#include "core/exr_reader.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStringAttribute.h>

#include <cstddef>
#include <exception>
#include <sstream>
#include <unordered_map>

namespace {

std::string buildChannelName(const Imf::Header& header, const char* rawName) {
    const std::string channelName = rawName != nullptr ? rawName : "";
    if (channelName.find('.') != std::string::npos) {
        return channelName;
    }

    if (const auto* nameAttribute = header.findTypedAttribute<Imf::StringAttribute>("name")) {
        const std::string& partName = nameAttribute->value();
        if (!partName.empty()) {
            return partName + "." + channelName;
        }
    }

    return channelName;
}

std::string makeLegacyAlias(const std::string& channelName) {
    const auto remap = [&](const std::string& prefix, const std::string& replacement) -> std::string {
        if (channelName.rfind(prefix, 0) != 0) {
            return {};
        }
        return replacement + channelName.substr(prefix.size());
    };

    if (const std::string alias = remap("Image.", "RenderLayer.Combined."); !alias.empty()) {
        return alias;
    }
    if (const std::string alias = remap("Normal.", "RenderLayer.Normal."); !alias.empty()) {
        return alias;
    }
    if (const std::string alias = remap("Vector.", "RenderLayer.Vector."); !alias.empty()) {
        return alias;
    }
    if (const std::string alias = remap("Diffuse Color.", "RenderLayer.DiffCol."); !alias.empty()) {
        return alias;
    }
    if (const std::string alias = remap("Glossy Color.", "RenderLayer.GlossCol."); !alias.empty()) {
        return alias;
    }
    if (channelName == "Depth.V") {
        return "RenderLayer.Depth.Z";
    }
    if (channelName == "Roughness.V") {
        return "RenderLayer.Roughness.X";
    }
    return {};
}

} // namespace

struct ExrReader::Impl {
    int width = 0;
    int height = 0;
    std::vector<std::string> channelOrder;
    std::unordered_map<std::string, std::vector<float>> channelData;
    std::unordered_map<std::string, std::string> aliases;
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
    m_impl->aliases.clear();
}

int ExrReader::width() const {
    return m_impl ? m_impl->width : 0;
}

int ExrReader::height() const {
    return m_impl ? m_impl->height : 0;
}

bool ExrReader::open(const std::string& path, std::string& errorMsg) {
    close();

    try {
        Imf::MultiPartInputFile file(path.c_str());
        const int partCount = file.parts();
        if (partCount <= 0) {
            errorMsg = "EXR contains no readable parts";
            return false;
        }

        for (int partIndex = 0; partIndex < partCount; ++partIndex) {
            Imf::InputPart part(file, partIndex);
            const Imf::Header& header = part.header();
            const Imath::Box2i dataWindow = header.dataWindow();
            const int partWidth = dataWindow.max.x - dataWindow.min.x + 1;
            const int partHeight = dataWindow.max.y - dataWindow.min.y + 1;

            if (partWidth <= 0 || partHeight <= 0) {
                std::ostringstream stream;
                stream << "Part " << partIndex << " has invalid data window";
                errorMsg = stream.str();
                close();
                return false;
            }

            if (m_impl->width == 0 && m_impl->height == 0) {
                m_impl->width = partWidth;
                m_impl->height = partHeight;
            } else if (m_impl->width != partWidth || m_impl->height != partHeight) {
                std::ostringstream stream;
                stream << "Part " << partIndex << " dimensions " << partWidth << "x" << partHeight
                       << " do not match first part dimensions " << m_impl->width << "x" << m_impl->height;
                errorMsg = stream.str();
                close();
                return false;
            }

            const Imf::ChannelList& channels = header.channels();
            size_t channelCount = 0;
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                ++channelCount;
            }

            const size_t pixelCount = static_cast<size_t>(partWidth) * static_cast<size_t>(partHeight);
            std::vector<std::pair<std::string, std::vector<float>>> loadedChannels;
            loadedChannels.reserve(channelCount);

            Imf::FrameBuffer frameBuffer;
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                const std::string actualName = buildChannelName(header, it.name());
                loadedChannels.emplace_back(actualName, std::vector<float>(pixelCount, 0.0f));

                auto& channelPixels = loadedChannels.back().second;
                const std::ptrdiff_t baseOffset = static_cast<std::ptrdiff_t>(dataWindow.min.x) +
                                                  static_cast<std::ptrdiff_t>(dataWindow.min.y) *
                                                      static_cast<std::ptrdiff_t>(partWidth);
                frameBuffer.insert(
                    it.name(),
                    Imf::Slice(Imf::FLOAT,
                               reinterpret_cast<char*>(channelPixels.data() - baseOffset),
                               sizeof(float),
                               sizeof(float) * static_cast<size_t>(partWidth)));
            }

            part.setFrameBuffer(frameBuffer);
            part.readPixels(dataWindow.min.y, dataWindow.max.y);

            for (auto& [name, pixels] : loadedChannels) {
                auto inserted = m_impl->channelData.emplace(name, std::move(pixels));
                if (!inserted.second) {
                    errorMsg = "Duplicate EXR channel name: " + name;
                    close();
                    return false;
                }

                m_impl->channelOrder.push_back(name);
                const std::string alias = makeLegacyAlias(name);
                if (!alias.empty() && alias != name) {
                    m_impl->aliases.emplace(alias, name);
                }
            }
        }

        errorMsg.clear();
        return true;
    } catch (const std::exception& ex) {
        close();
        errorMsg = ex.what();
        return false;
    }
}

std::vector<std::string> ExrReader::listChannels() const {
    return m_impl ? m_impl->channelOrder : std::vector<std::string>{};
}

const float* ExrReader::readChannel(const std::string& name) const {
    if (!m_impl) {
        return nullptr;
    }

    auto it = m_impl->channelData.find(name);
    if (it != m_impl->channelData.end() && !it->second.empty()) {
        return it->second.data();
    }

    const auto aliasIt = m_impl->aliases.find(name);
    if (aliasIt == m_impl->aliases.end()) {
        return nullptr;
    }

    it = m_impl->channelData.find(aliasIt->second);
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
