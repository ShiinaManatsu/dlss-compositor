#include "ui/image_viewer.h"

#include "gpu/texture_pipeline.h"
#include "core/exr_reader.h"
#include "core/channel_mapper.h"

#include <backends/imgui_impl_vulkan.h>
#include <stdexcept>
#include <iostream>

ImageViewer::ImageViewer() {
}

ImageViewer::~ImageViewer() {
    unload();
}

void ImageViewer::load(const std::string& exrPath, VkDevice device, VmaAllocator allocator, TexturePipeline& pipeline) {
    unload();

    m_device = device;
    m_allocator = allocator;
    m_pipeline = &pipeline;
    m_filename = exrPath;
    m_selectedChannel = Channel::Color;
    m_zoom = 1.0f;
    m_splitPos = 0.5f;

    createSampler();
    createPlaceholder();

    ExrReader reader;
    std::string errorMsg;
    if (!reader.open(exrPath, errorMsg)) {
        std::cerr << "ImageViewer: Failed to open EXR: " << exrPath << " (" << errorMsg << ")\n";
        return;
    }

    m_width = reader.width();
    m_height = reader.height();
    m_loaded = true;

    ChannelMapper mapper;
    MappedBuffers buffers;
    if (!mapper.mapFromExr(reader, buffers, errorMsg)) {
        std::cerr << "ImageViewer: Failed to map channels: " << errorMsg << "\n";
        return; // Mapped buffers might be incomplete
    }

    size_t pixelCount = m_width * m_height;

    auto uploadChannel = [&](Channel cEnum, const std::vector<float>& data, int srcChannels, int dstChannels, VkFormat format, bool isDepth = false, bool isMV = false) {
        m_channels[static_cast<int>(cEnum)].exists = false;
        if (data.empty()) return;

        std::vector<float> uploadData(pixelCount * dstChannels, 0.0f);
        
        if (isDepth) {
            float minZ = 1e30f;
            float maxZ = -1e30f;
            for (size_t i = 0; i < pixelCount; ++i) {
                float v = data[i];
                if (v > 0.0f && v < 1e10f) {
                    if (v < minZ) minZ = v;
                    if (v > maxZ) maxZ = v;
                }
            }
            if (maxZ > minZ) {
                for (size_t i = 0; i < pixelCount; ++i) {
                    float v = data[i];
                    if (v <= 0.0f) uploadData[i] = 0.0f;
                    else if (v >= 1e10f) uploadData[i] = 1.0f;
                    else uploadData[i] = (v - minZ) / (maxZ - minZ);
                }
            } else {
                uploadData = data; // fallback
            }
        } else if (isMV) {
            // MV: display XY as RG. MV data from mapper is 4 channels.
            for (size_t i = 0; i < pixelCount; ++i) {
                uploadData[i * dstChannels + 0] = data[i * srcChannels + 0]; // R
                uploadData[i * dstChannels + 1] = data[i * srcChannels + 1]; // G
                if (dstChannels > 2) {
                    uploadData[i * dstChannels + 2] = 0.0f; // B
                    uploadData[i * dstChannels + 3] = 1.0f; // A
                }
            }
        } else {
            for (size_t i = 0; i < pixelCount; ++i) {
                for (int c = 0; c < dstChannels; ++c) {
                    if (c < srcChannels) {
                        uploadData[i * dstChannels + c] = data[i * srcChannels + c];
                    } else if (c == 3) {
                        uploadData[i * dstChannels + c] = 1.0f; // Alpha = 1
                    }
                }
            }
        }

        TextureHandle* handle = new TextureHandle();
        *handle = m_pipeline->upload(uploadData.data(), m_width, m_height, dstChannels, format);
        VkDescriptorSet imguiTex = ImGui_ImplVulkan_AddTexture(m_sampler, handle->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        
        m_channels[static_cast<int>(cEnum)].handle = handle;
        m_channels[static_cast<int>(cEnum)].imguiTexture = imguiTex;
        m_channels[static_cast<int>(cEnum)].exists = true;
    };

    uploadChannel(Channel::Color, buffers.color, 4, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    uploadChannel(Channel::Depth, buffers.depth, 1, 1, VK_FORMAT_R32_SFLOAT, true);
    uploadChannel(Channel::Normal, buffers.normals, 3, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    uploadChannel(Channel::MotionVectors, buffers.motionVectors, 4, 2, VK_FORMAT_R16G16_SFLOAT, false, true);
    uploadChannel(Channel::DiffAlbedo, buffers.diffuseAlbedo, 3, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    uploadChannel(Channel::SpecAlbedo, buffers.specularAlbedo, 3, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    uploadChannel(Channel::Roughness, buffers.roughness, 1, 1, VK_FORMAT_R32_SFLOAT);
}

void ImageViewer::unload() {
    if (m_pipeline && m_loaded) {
        for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
            if (m_channels[i].exists) {
                ImGui_ImplVulkan_RemoveTexture(m_channels[i].imguiTexture);
                m_pipeline->destroy(*m_channels[i].handle);
                delete m_channels[i].handle;
            }
            m_channels[i].exists = false;
            m_channels[i].imguiTexture = VK_NULL_HANDLE;
            m_channels[i].handle = nullptr;
        }

        if (m_blackPlaceholder) {
            ImGui_ImplVulkan_RemoveTexture(m_blackImguiTexture);
            m_pipeline->destroy(*m_blackPlaceholder);
            delete m_blackPlaceholder;
            m_blackPlaceholder = nullptr;
            m_blackImguiTexture = VK_NULL_HANDLE;
        }

        if (m_sampler != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }
    }
    m_loaded = false;
    m_pipeline = nullptr;
}

void ImageViewer::createSampler() {
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sampler for ImGui textures.");
    }
}

void ImageViewer::createPlaceholder() {
    float blackPixel[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_blackPlaceholder = new TextureHandle();
    *m_blackPlaceholder = m_pipeline->upload(blackPixel, 1, 1, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
    m_blackImguiTexture = ImGui_ImplVulkan_AddTexture(m_sampler, m_blackPlaceholder->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

const char* ImageViewer::getChannelName(Channel c) const {
    switch (c) {
        case Channel::Color: return "Color";
        case Channel::Depth: return "Depth";
        case Channel::Normal: return "Normal";
        case Channel::MotionVectors: return "MotionVectors";
        case Channel::DiffAlbedo: return "DiffAlbedo";
        case Channel::SpecAlbedo: return "SpecAlbedo";
        case Channel::Roughness: return "Roughness";
        default: return "Unknown";
    }
}

void ImageViewer::render(ImVec2 availSize) {
    if (!m_loaded) {
        ImGui::Text("No image loaded.");
        return;
    }

    ImGui::BeginChild("TopBar", ImVec2(0, 40), false);
    
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Channel", getChannelName(m_selectedChannel))) {
        for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
            bool isSelected = (m_selectedChannel == static_cast<Channel>(i));
            if (ImGui::Selectable(getChannelName(static_cast<Channel>(i)), isSelected)) {
                m_selectedChannel = static_cast<Channel>(i);
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Fit to Window")) {
        float zoomW = availSize.x / (float)m_width;
        float zoomH = (availSize.y - 70) / (float)m_height;
        m_zoom = std::min(zoomW, zoomH);
    }
    
    ImGui::SameLine();
    ImGui::Text("Zoom: %.2fx", m_zoom);

    ImGui::EndChild();

    ImGui::BeginChild("ImageArea", ImVec2(0, availSize.y - 70), true, ImGuiWindowFlags_HorizontalScrollbar);
    
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        m_zoom += ImGui::GetIO().MouseWheel * 0.1f;
        if (m_zoom < 0.1f) m_zoom = 0.1f;
        if (m_zoom > 20.0f) m_zoom = 20.0f;
    }

    ImVec2 imageSize((float)m_width * m_zoom, (float)m_height * m_zoom);
    
    VkDescriptorSet tex = m_channels[static_cast<int>(m_selectedChannel)].exists ? 
                          m_channels[static_cast<int>(m_selectedChannel)].imguiTexture : 
                          m_blackImguiTexture;

    if (m_selectedChannel == Channel::Color) {
        renderSplitView(imageSize, tex, tex);
    } else {
        ImGui::Image((ImTextureID)tex, imageSize);
    }

    ImGui::EndChild();

    ImGui::BeginChild("BottomBar", ImVec2(0, 0), false);
    ImGui::Text("%s  %dx%d  Channel: %s", 
                m_filename.c_str(), m_width, m_height, getChannelName(m_selectedChannel));
    ImGui::EndChild();
}

void ImageViewer::renderSplitView(ImVec2 size, VkDescriptorSet texLeft, VkDescriptorSet texRight) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)texLeft, 
        pos, 
        ImVec2(pos.x + size.x * m_splitPos, pos.y + size.y),
        ImVec2(0, 0), 
        ImVec2(m_splitPos, 1.0f)
    );

    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)texRight, 
        ImVec2(pos.x + size.x * m_splitPos, pos.y), 
        ImVec2(pos.x + size.x, pos.y + size.y),
        ImVec2(m_splitPos, 0.0f), 
        ImVec2(1.0f, 1.0f)
    );

    ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x * m_splitPos - 5.0f, pos.y));
    ImGui::InvisibleButton("Splitter", ImVec2(10.0f, size.y));
    if (ImGui::IsItemActive()) {
        m_splitPos += ImGui::GetIO().MouseDelta.x / size.x;
        if (m_splitPos < 0.05f) m_splitPos = 0.05f;
        if (m_splitPos > 0.95f) m_splitPos = 0.95f;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(pos.x + size.x * m_splitPos, pos.y),
        ImVec2(pos.x + size.x * m_splitPos, pos.y + size.y),
        IM_COL32(255, 255, 255, 255), 
        2.0f
    );
    
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y));
}
