#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dlss {

inline constexpr const char* kDefaultPreviewSharedMemoryName = "Local\\DLSSCompositorPreview";

struct PreviewFrameRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

PreviewFrameRgba8 makePreviewRgba8FromRgba16f(const std::vector<std::uint8_t>& rgba16f,
                                              std::uint32_t width,
                                              std::uint32_t height);

class PreviewPublisher {
public:
    explicit PreviewPublisher(std::string sharedMemoryName);
    ~PreviewPublisher();

    PreviewPublisher(const PreviewPublisher&) = delete;
    PreviewPublisher& operator=(const PreviewPublisher&) = delete;

    bool enabled() const noexcept;
    void publishRgba16f(const std::vector<std::uint8_t>& rgba16f,
                        std::uint32_t width,
                        std::uint32_t height);

private:
    std::string m_sharedMemoryName;

#ifdef _WIN32
    void ensureMapping();
    void closeMapping() noexcept;

    void* m_mapping = nullptr;
    void* m_view = nullptr;
    std::uint32_t m_generation = 0;
#endif
};

} // namespace dlss
