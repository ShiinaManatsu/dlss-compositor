#include "ngx_wrapper.h"

#include "../core/log.h"

// DLSS NGX headers — Vulkan path.
// All NGX API calls MUST have // DLSS §X.X comments per plan Must Have requirement.
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dlss {

namespace {

std::string getDllVersion(const wchar_t* dllName) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(dllName, &dummy);
    if (size == 0) return "unknown";

    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(dllName, 0, size, data.data())) return "unknown";

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len)) return "unknown";
    if (!ffi) return "unknown";

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return buf;
}

const wchar_t kSrDll[] = L"nvngx_dlss.dll";
const wchar_t kFgDll[] = L"nvngx_dlssg.dll";
const wchar_t kRrDll[] = L"nvngx_dlssd.dll";

} // anonymous namespace

NgxContext::NgxContext(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device)
    : m_device(device)
{
    // DLSS SR §3.5 — Init with ProjectID is recommended over legacy Init.
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        "d4a7ef3e-2d2d-4e9d-b8bb-2d773ef3b2fd",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "1.0.0",
        L".",
        instance,
        physicalDevice,
        device,
        nullptr,
        nullptr,
        nullptr,
        NVSDK_NGX_Version_API);

    if (NVSDK_NGX_FAILED(result)) { // DLSS SR §3.5
        throw std::runtime_error(
            std::string("NGX Init failed: NVSDK_NGX_Result=") + std::to_string(result));
    }

    // DLSS SR §5 — Get capability parameters (use GetCapabilityParameters, NOT GetParameters).
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_ngxParams);
    if (NVSDK_NGX_FAILED(result)) {
        // DLSS SR §5.4 — Shut down the initialized NGX device on capability query failure.
        (void)NVSDK_NGX_VULKAN_Shutdown1(device);
        throw std::runtime_error("NVSDK_NGX_VULKAN_GetCapabilityParameters failed");
    }

    queryCapabilities();
    m_dllVersions = queryDllVersions();

    LOG_INFO("[NGX] SR: {} | FG: {} max={}x | RR: {} | "
             "nvngx_dlss.dll v{} | nvngx_dlssg.dll v{} | nvngx_dlssd.dll v{}",
        m_srAvailable ? "AVAILABLE" : "UNAVAILABLE",
        m_fgAvailable ? "AVAILABLE" : "UNAVAILABLE",
        m_fgMaxMultiplier,
        m_rrAvailable ? "AVAILABLE" : "UNAVAILABLE",
        m_dllVersions.sr,
        m_dllVersions.fg,
        m_dllVersions.rr);
}

NgxContext::~NgxContext() {
    if (m_ngxParams) {
        // DLSS SR §5 — Destroy capability parameters before shutdown.
        (void)NVSDK_NGX_VULKAN_DestroyParameters(m_ngxParams);
        m_ngxParams = nullptr;
    }

    if (m_device != VK_NULL_HANDLE) {
        // DLSS SR §5.4 — Shutdown using device handle; destructor must not throw.
        (void)NVSDK_NGX_VULKAN_Shutdown1(m_device);
        m_device = VK_NULL_HANDLE;
    }
}

void NgxContext::queryCapabilities() {
    int srAvail = 0;
    // DLSS SR §5 — Query SuperSampling availability.
    NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetI(
        m_ngxParams,
        NVSDK_NGX_Parameter_SuperSampling_Available,
        &srAvail);
    m_srAvailable = (r == NVSDK_NGX_Result_Success) && (srAvail != 0);

    int fgAvail = 0;
    // DLSS §1 FG — Query FrameGeneration availability.
    r = NVSDK_NGX_Parameter_GetI(
        m_ngxParams,
        NVSDK_NGX_Parameter_FrameGeneration_Available,
        &fgAvail);
    m_fgAvailable = (r == NVSDK_NGX_Result_Success) && (fgAvail != 0);

    int fgMaxGeneratedFrames = 0;
    // DLSS FG §4 — MultiFrameCountMax is generated-frame count: 1 => 2x, 3 => 4x.
    r = NVSDK_NGX_Parameter_GetI(
        m_ngxParams,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
        &fgMaxGeneratedFrames);
    if (m_fgAvailable) {
        m_fgMaxMultiplier = (r == NVSDK_NGX_Result_Success)
            ? std::max(2, fgMaxGeneratedFrames + 1)
            : 2;
    } else {
        m_fgMaxMultiplier = 0;
    }

    int rrAvail = 0;
    // nvsdk_ngx_defs_dlssd.h — SuperSamplingDenoising.Available (not SuperSampling.Available)
    r = NVSDK_NGX_Parameter_GetI(
        m_ngxParams,
        NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, // DLSS-RR §capability
        &rrAvail);
    m_rrAvailable = (r == NVSDK_NGX_Result_Success) && (rrAvail != 0);
}

NgxDllVersions NgxContext::queryDllVersions() const {
    return {
        getDllVersion(kSrDll),
        getDllVersion(kFgDll),
        getDllVersion(kRrDll),
    };
}

DlssOptimalSettings NgxContext::queryDlssOptimalSettings(std::uint32_t outputWidth,
                                                         std::uint32_t outputHeight,
                                                         int perfQuality) const {
    DlssOptimalSettings settings{};
    float deprecatedSharpness = 0.0f;
    // DLSS SR §5.2.8 - query the exact creation resolution and the inclusive
    // dynamic-resolution range for this output size and quality mode.
    const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        m_ngxParams,
        outputWidth,
        outputHeight,
        static_cast<NVSDK_NGX_PerfQuality_Value>(perfQuality),
        &settings.optimalWidth,
        &settings.optimalHeight,
        &settings.maxWidth,
        &settings.maxHeight,
        &settings.minWidth,
        &settings.minHeight,
        &deprecatedSharpness);
    if (NVSDK_NGX_FAILED(result)) { // DLSS SR §5.2.8
        throw std::runtime_error(
            "NGX_DLSS_GET_OPTIMAL_SETTINGS failed: " + std::to_string(result));
    }
    settings.supported = settings.optimalWidth > 0 && settings.optimalHeight > 0;
    return settings;
}

} // namespace dlss
