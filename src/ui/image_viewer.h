#pragma once

#include <string>
#include <vector>

#include <volk.h>
#include <imgui.h>

struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;

class TexturePipeline;
struct TextureHandle;

class ImageViewer {
public:
    ImageViewer();
    ~ImageViewer();

    void load(const std::string& exrPath, VkDevice device, VmaAllocator allocator, TexturePipeline& pipeline);
    void setOutputPath(const std::string& exrPath, VkDevice device, VmaAllocator allocator, TexturePipeline& pipeline);
    void unload();
    void render(ImVec2 availSize);

    bool isLoaded() const { return m_loaded; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    TexturePipeline* m_pipeline = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;

    bool m_loaded = false;

    enum class Channel {
        Color,
        Depth,
        Normal,
        MotionVectors,
        DiffAlbedo,
        SpecAlbedo,
        Roughness,
        Count
    };

    struct ChannelData {
        TextureHandle* handle = nullptr;
        VkDescriptorSet imguiTexture = VK_NULL_HANDLE;
        bool exists = false;
    };

    ChannelData m_channels[static_cast<int>(Channel::Count)];
    Channel m_selectedChannel = Channel::Color;

    ChannelData m_outputColorChannel;
    bool m_outputLoaded = false;

    TextureHandle* m_blackPlaceholder = nullptr;
    VkDescriptorSet m_blackImguiTexture = VK_NULL_HANDLE;

    std::string m_filename;
    int m_width = 0;
    int m_height = 0;
    float m_zoom = 1.0f;
    float m_splitPos = 0.5f;

    void createSampler();
    void createPlaceholder();
    const char* getChannelName(Channel c) const;
    void renderSplitView(ImVec2 size, VkDescriptorSet texLeft, VkDescriptorSet texRight);
};
