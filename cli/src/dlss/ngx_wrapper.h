#pragma once

#include <stdexcept>
#include <string>
#include <cstdint>

#include <vulkan/vulkan.h>

// Forward declare NGX types to avoid including heavy NGX headers in header.
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace dlss {

struct NgxDllVersions {
    std::string sr;   // nvngx_dlss.dll version string
    std::string fg;   // nvngx_dlssg.dll version string
    std::string rr;   // nvngx_dlssd.dll version string
};

struct DlssOptimalSettings {
    bool supported = false;
    std::uint32_t optimalWidth = 0;
    std::uint32_t optimalHeight = 0;
    std::uint32_t minWidth = 0;
    std::uint32_t minHeight = 0;
    std::uint32_t maxWidth = 0;
    std::uint32_t maxHeight = 0;
};

/// Wraps NVIDIA NGX SDK initialization, capability query, and shutdown.
/// Per DLSS SR §3.5 + FG §1: must call Init before creating any DLSS feature.
class NgxContext {
public:
    /// Initialize NGX on the given Vulkan device.
    /// Throws std::runtime_error if NGX unavailable.
    NgxContext(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
    ~NgxContext();

    // Non-copyable
    NgxContext(const NgxContext&) = delete;
    NgxContext& operator=(const NgxContext&) = delete;

    // Capability queries
    bool isSrAvailable() const { return m_srAvailable; }
    bool isFgAvailable() const { return m_fgAvailable; }
    bool isRrAvailable() const { return m_rrAvailable; }  // nvsdk_ngx_defs_dlssd.h SuperSamplingDenoising.Available
    int  getFgMaxMultiplier() const { return m_fgMaxMultiplier; }

    DlssOptimalSettings queryDlssOptimalSettings(std::uint32_t outputWidth,
                                                 std::uint32_t outputHeight,
                                                 int perfQuality) const;

    // DLL version info (queried at runtime from loaded DLLs)
    NgxDllVersions getDllVersions() const { return m_dllVersions; }

    // Raw NGX parameter access (needed by DlssSrPass, DlssFgPass)
    NVSDK_NGX_Parameter* params() const { return m_ngxParams; }

private:
    void queryCapabilities();
    NgxDllVersions queryDllVersions() const;

    VkDevice m_device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* m_ngxParams = nullptr;
    bool m_srAvailable = false;
    bool m_fgAvailable = false;
    bool m_rrAvailable = false;
    int  m_fgMaxMultiplier = 0;
    NgxDllVersions m_dllVersions;
};

} // namespace dlss
