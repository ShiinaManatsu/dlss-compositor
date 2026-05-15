#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "dlss/ngx_wrapper.h"

#include "core/logger.h"
#include "core/version.h"

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_vk.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "Version.lib")

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kNgxWorkDir[] = L".";

void NVSDK_CONV ngxLogCallback(const char* message,
                               NVSDK_NGX_Logging_Level,
                               NVSDK_NGX_Feature) {
    Log::error("[NGX] %s\n", message != nullptr ? message : "(null)");
}

std::string wideToUtf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return "unknown";
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return "unknown";
    }

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string ngxResultToString(NVSDK_NGX_Result result) {
    return wideToUtf8(GetNGXResultAsString(result));
}

bool readIntParameter(const NVSDK_NGX_Parameter* parameters, const char* name, int& value) {
    if (parameters == nullptr || name == nullptr) {
        return false;
    }

    int localValue = 0;
    const NVSDK_NGX_Result result = parameters->Get(name, &localValue);
    if (NVSDK_NGX_FAILED(result)) {
        return false;
    }

    value = localValue;
    return true;
}

static int resolveAutoPreset(NVSDK_NGX_PerfQuality_Value quality) {
    switch (quality) {
    case NVSDK_NGX_PerfQuality_Value_DLAA:
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
    case NVSDK_NGX_PerfQuality_Value_Balanced:
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    }

    return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

const char* qualityName(DlssQualityMode quality) {
    switch (quality) {
    case DlssQualityMode::DLAA: return "DLAA";
    case DlssQualityMode::MaxQuality: return "MaxQuality";
    case DlssQualityMode::Balanced: return "Balanced";
    case DlssQualityMode::Performance: return "Performance";
    case DlssQualityMode::UltraPerformance: return "UltraPerformance";
    }

    return "unknown";
}

const char* presetLabel(int presetValue) {
    switch (presetValue) {
    case NVSDK_NGX_DLSS_Hint_Render_Preset_J: return "J";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_K: return "K";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_L: return "L";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_M: return "M";
    default: return "?";
    }
}

DlssSRPreset presetOverrideForQuality(NVSDK_NGX_PerfQuality_Value quality,
                                      const DlssPresetConfig& presets) {
    switch (quality) {
    case NVSDK_NGX_PerfQuality_Value_DLAA:
        return presets.dlaa;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        return presets.quality;
    case NVSDK_NGX_PerfQuality_Value_Balanced:
        return presets.balanced;
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        return presets.performance;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        return presets.ultraPerformance;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        return presets.ultraQuality;
    }

    return DlssSRPreset::Auto;
}

int presetSlotForQuality(NVSDK_NGX_PerfQuality_Value quality,
                         const DlssPresetSlots& slots) {
    switch (quality) {
    case NVSDK_NGX_PerfQuality_Value_DLAA:
        return slots.dlaa;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        return slots.quality;
    case NVSDK_NGX_PerfQuality_Value_Balanced:
        return slots.balanced;
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        return slots.performance;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        return slots.ultraPerformance;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        return slots.ultraQuality;
    }

    return slots.quality;
}

const char* presetSourceLabel(DlssSRPreset perQualityPreset, DlssSRPreset globalPreset) {
    if (perQualityPreset != DlssSRPreset::Auto) {
        return "per-quality override";
    }
    if (globalPreset != DlssSRPreset::Auto) {
        return "global override";
    }
    return "auto";
}

static std::string getDllFileVersion(const wchar_t* dllName) {
    wchar_t dllPath[MAX_PATH] = {};
    HMODULE hMod = GetModuleHandleW(dllName);
    if (hMod == nullptr) {
        return "not loaded";
    }
    if (GetModuleFileNameW(hMod, dllPath, MAX_PATH) == 0) {
        return "path unknown";
    }

    DWORD dummy = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(dllPath, &dummy);
    if (infoSize == 0) {
        return "version unavailable";
    }

    std::vector<BYTE> versionData(infoSize);
    if (!GetFileVersionInfoW(dllPath, 0, infoSize, versionData.data())) {
        return "version unavailable";
    }

    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT fileInfoSize = 0;
    if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&fileInfo), &fileInfoSize) ||
        fileInfo == nullptr) {
        return "version unavailable";
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  HIWORD(fileInfo->dwFileVersionMS),
                  LOWORD(fileInfo->dwFileVersionMS),
                  HIWORD(fileInfo->dwFileVersionLS),
                  LOWORD(fileInfo->dwFileVersionLS));
    return buf;
}

} // namespace

DlssPresetConfig makeDlssPresetConfig(const AppConfig& config) {
    DlssPresetConfig presets;
    presets.globalPreset = config.preset;
    presets.dlaa = config.presetDLAA;
    presets.quality = config.presetQuality;
    presets.balanced = config.presetBalanced;
    presets.performance = config.presetPerformance;
    presets.ultraPerformance = config.presetUltraPerformance;
    presets.ultraQuality = config.presetUltraQuality;
    return presets;
}

int resolveDlssPresetValue(DlssSRPreset perQualityPreset,
                           DlssSRPreset globalPreset,
                           NVSDK_NGX_PerfQuality_Value quality) {
    if (perQualityPreset != DlssSRPreset::Auto) {
        return static_cast<int>(perQualityPreset);
    }
    if (globalPreset != DlssSRPreset::Auto) {
        return static_cast<int>(globalPreset);
    }
    return resolveAutoPreset(quality);
}

DlssPresetSlots resolveDlssPresetSlots(const DlssPresetConfig& presets) {
    DlssPresetSlots slots;
    slots.dlaa = resolveDlssPresetValue(presets.dlaa,
                                        presets.globalPreset,
                                        NVSDK_NGX_PerfQuality_Value_DLAA);
    slots.quality = resolveDlssPresetValue(presets.quality,
                                           presets.globalPreset,
                                           NVSDK_NGX_PerfQuality_Value_MaxQuality);
    slots.balanced = resolveDlssPresetValue(presets.balanced,
                                            presets.globalPreset,
                                            NVSDK_NGX_PerfQuality_Value_Balanced);
    slots.performance = resolveDlssPresetValue(presets.performance,
                                               presets.globalPreset,
                                               NVSDK_NGX_PerfQuality_Value_MaxPerf);
    slots.ultraPerformance = resolveDlssPresetValue(presets.ultraPerformance,
                                                    presets.globalPreset,
                                                    NVSDK_NGX_PerfQuality_Value_UltraPerformance);
    slots.ultraQuality = resolveDlssPresetValue(presets.ultraQuality,
                                                presets.globalPreset,
                                                NVSDK_NGX_PerfQuality_Value_UltraQuality);
    return slots;
}

NgxContext::NgxContext() = default;

NgxContext::~NgxContext() {
    shutdown();
}

bool NgxContext::init(VkInstance instance,
                      VkPhysicalDevice physicalDevice,
                      VkDevice device,
                      VkCommandBuffer cmdBuf,
                      std::string& errorMsg,
                      NVSDK_NGX_Logging_Level logLevel) {
    (void)cmdBuf;

    if (m_initialized) {
        return true;
    }

    shutdown();
    errorMsg.clear();
    m_device = device;
    m_dlssSRAvailable = false;
    m_dlssFGAvailable = false;
    m_maxMultiFrameCount = 0;
    m_unavailableReason.clear();

    NVSDK_NGX_FeatureCommonInfo featureInfo{};
    featureInfo.LoggingInfo.LoggingCallback = ngxLogCallback;
    featureInfo.LoggingInfo.MinimumLoggingLevel = logLevel;
    featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;

    const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(appver::kNgxProjectId,
                                                                               NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                               appver::kEngineVersion,
                                                                              kNgxWorkDir,
                                                                              instance,
                                                                              physicalDevice,
                                                                              device,
                                                                              vkGetInstanceProcAddr,
                                                                              vkGetDeviceProcAddr,
                                                                              &featureInfo,
                                                                              NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(initResult)) {
        errorMsg = "NGX init failed: " + ngxResultToString(initResult);
        m_device = nullptr;
        return false;
    }

    const NVSDK_NGX_Result paramsResult = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_parameters);
    if (NVSDK_NGX_FAILED(paramsResult)) {
        errorMsg = "Failed to get NGX capability parameters: " + ngxResultToString(paramsResult);
        shutdown();
        return false;
    }

    m_initialized = true;

    // --- Startup banner (parseable by GUI regex) ---
    Log::info("[NGX] SDK: %s (compile-time)\n", appver::kDlssSdkVersion);
    Log::info("[NGX] Engine: %s %s\n", appver::kEngineName, appver::kEngineVersion);

    queryDlssSRAvailability();
    queryDlssFGAvailability();

    if (m_dlssSRAvailable) {
        Log::info("[NGX] DLSS-SR: available\n");
    } else {
        Log::info("[NGX] DLSS-SR: unavailable (%s)\n", m_unavailableReason.c_str());
    }

    if (m_dlssFGAvailable) {
        Log::info("[NGX] DLSS-FG: available, max-multi-frame=%d\n", m_maxMultiFrameCount);
    } else {
        Log::info("[NGX] DLSS-FG: unavailable\n");
    }

    // Min driver requirement
    int driverMajor = 0, driverMinor = 0;
    if (readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, driverMajor) &&
        readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, driverMinor)) {
        Log::info("[NGX] Min driver: %d.%d\n", driverMajor, driverMinor);
    }

    // DLL file versions (Windows — actual runtime DLL may be newer than SDK compile-time version)
    const std::string srDllVersion = getDllFileVersion(L"nvngx_dlss.dll");
    const std::string fgDllVersion = getDllFileVersion(L"nvngx_dlssg.dll");
    Log::info("[NGX] Loaded: nvngx_dlss.dll v%s, nvngx_dlssg.dll v%s\n",
              srDllVersion.c_str(), fgDllVersion.c_str());

    return true;
}

bool NgxContext::createDlssSR(int inputWidth,
                              int inputHeight,
                              int outputWidth,
                              int outputHeight,
                              DlssQualityMode quality,
                              const DlssPresetConfig& presets,
                              VkCommandBuffer cmdBuf,
                              std::string& errorMsg) {
    errorMsg.clear();

    if (!m_initialized || m_parameters == nullptr) {
        errorMsg = "NGX is not initialized.";
        return false;
    }

    if (!m_dlssSRAvailable) {
        errorMsg = m_unavailableReason.empty() ? "DLSS-SR is not available." : m_unavailableReason;
        return false;
    }

    releaseDlssSR();

    NVSDK_NGX_DLSS_Create_Params createParams{};
    createParams.Feature.InWidth = static_cast<unsigned int>(inputWidth);
    createParams.Feature.InHeight = static_cast<unsigned int>(inputHeight);
    createParams.Feature.InTargetWidth = static_cast<unsigned int>(outputWidth);
    createParams.Feature.InTargetHeight = static_cast<unsigned int>(outputHeight);
    const NVSDK_NGX_PerfQuality_Value perfQuality = mapQuality(quality);
    createParams.Feature.InPerfQualityValue = perfQuality;
     createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                         NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                         NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    createParams.InEnableOutputSubrects = false;

    const DlssPresetSlots presetSlots = resolveDlssPresetSlots(presets);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,             presetSlots.dlaa);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,          presetSlots.quality);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,         presetSlots.balanced);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,      presetSlots.performance);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, presetSlots.ultraPerformance);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,     presetSlots.ultraQuality);

    const int effectivePresetValue = presetSlotForQuality(perfQuality, presetSlots);
    const DlssSRPreset perQualityPreset = presetOverrideForQuality(perfQuality, presets);
    Log::info("[NGX] DLSS-SR preset: %s (%s for %s)\n",
              presetLabel(effectivePresetValue),
              presetSourceLabel(perQualityPreset, presets.globalPreset),
              qualityName(quality));

    const NVSDK_NGX_Result createResult = NGX_VULKAN_CREATE_DLSS_EXT1(m_device,
                                                                        cmdBuf,
                                                                        1,
                                                                        1,
                                                                        &m_featureHandle,
                                                                        m_parameters,
                                                                        &createParams);
    if (NVSDK_NGX_FAILED(createResult)) {
        errorMsg = "Failed to create DLSS-SR feature: " + ngxResultToString(createResult);
        m_featureHandle = nullptr;
        return false;
    }

    Log::info("[NGX] DLSS-SR feature created: %dx%d -> %dx%d, quality=%s, preset=%s\n",
              inputWidth, inputHeight, outputWidth, outputHeight, qualityName(quality), presetLabel(effectivePresetValue));

    return true;
}

bool NgxContext::createDlssFG(unsigned int width,
                              unsigned int height,
                              unsigned int backbufferFormat,
                              VkCommandBuffer cmdBuf,
                              std::string& errorMsg) {
    errorMsg.clear();

    if (!m_initialized) {
        errorMsg = "NGX is not initialized.";
        return false;
    }

    if (m_parameters == nullptr) {
        errorMsg = "NGX capability parameters are unavailable.";
        return false;
    }

    if (!m_dlssFGAvailable) {
        errorMsg = "DLSS-G is not available.";
        return false;
    }

    releaseDlssFG();

    NVSDK_NGX_DLSSG_Create_Params createParams{};
    createParams.Width = width;
    createParams.Height = height;
    createParams.NativeBackbufferFormat = backbufferFormat;
    createParams.RenderWidth = width;
    createParams.RenderHeight = height;
    createParams.DynamicResolutionScaling = false;

    const NVSDK_NGX_Result createResult = NGX_VK_CREATE_DLSSG(cmdBuf,
                                                              1,
                                                              1,
                                                              &m_fgFeatureHandle,
                                                              m_parameters,
                                                              &createParams);
    if (NVSDK_NGX_FAILED(createResult)) {
        errorMsg = "Failed to create DLSS-G feature: " + ngxResultToString(createResult);
        m_fgFeatureHandle = nullptr;
        return false;
    }

    return true;
}

void NgxContext::releaseDlssSR() {
    if (m_featureHandle == nullptr) {
        return;
    }

    NVSDK_NGX_VULKAN_ReleaseFeature(m_featureHandle);
    m_featureHandle = nullptr;
}

void NgxContext::releaseDlssFG() {
    if (m_fgFeatureHandle == nullptr) {
        return;
    }

    NVSDK_NGX_VULKAN_ReleaseFeature(m_fgFeatureHandle);
    m_fgFeatureHandle = nullptr;
}

void NgxContext::shutdown() {
    releaseDlssFG();
    releaseDlssSR();

    if (m_parameters != nullptr) {
        NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
        m_parameters = nullptr;
    }

    if (m_initialized) {
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
    }

    m_device = nullptr;
    m_initialized = false;
    m_dlssSRAvailable = false;
    m_dlssFGAvailable = false;
    m_maxMultiFrameCount = 0;
    m_unavailableReason.clear();
}

NVSDK_NGX_Handle* NgxContext::featureHandle() const {
    return m_featureHandle;
}

NVSDK_NGX_Handle* NgxContext::fgFeatureHandle() const {
    return m_fgFeatureHandle;
}

NVSDK_NGX_Handle* NgxContext::getFeatureHandle() const {
    return m_featureHandle;
}

NVSDK_NGX_Parameter* NgxContext::parameters() const {
    return m_parameters;
}

int NgxContext::maxMultiFrameCount() const {
    return m_maxMultiFrameCount;
}

bool NgxContext::isInitialized() const {
    return m_initialized;
}

bool NgxContext::isDlssSRAvailable() const {
    return m_dlssSRAvailable;
}

bool NgxContext::isDlssFGAvailable() const {
    return m_dlssFGAvailable;
}

std::string NgxContext::unavailableReason() const {
    return m_unavailableReason;
}

NVSDK_NGX_PerfQuality_Value NgxContext::mapQuality(DlssQualityMode quality) const {
    switch (quality) {
    case DlssQualityMode::DLAA:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DlssQualityMode::MaxQuality:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case DlssQualityMode::Balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DlssQualityMode::Performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case DlssQualityMode::UltraPerformance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }

    return NVSDK_NGX_PerfQuality_Value_Balanced;
}

bool NgxContext::queryDlssSRAvailability() {
    if (m_parameters == nullptr) {
        m_dlssSRAvailable = false;
        m_unavailableReason = "NGX capability parameters are unavailable.";
        return false;
    }

    int available = 0;
    if (readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, available)) {
        m_dlssSRAvailable = available != 0;
    } else {
        m_dlssSRAvailable = false;
        m_unavailableReason = "DLSS-SR availability flag was not exposed by the NGX runtime.";
        return false;
    }

    if (m_dlssSRAvailable) {
        m_unavailableReason.clear();
        return true;
    }

    int needsUpdatedDriver = 0;
    int minDriverMajor = 0;
    int minDriverMinor = 0;
    int featureInitResult = 0;
    const bool hasNeedsDriver = readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, needsUpdatedDriver);
    const bool hasMinDriverMajor = readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, minDriverMajor);
    const bool hasMinDriverMinor = readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, minDriverMinor);
    const bool hasFeatureInitResult = readIntParameter(m_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, featureInitResult);

    m_unavailableReason = "DLSS-SR not available";
    if (hasNeedsDriver && needsUpdatedDriver != 0) {
        m_unavailableReason += " (driver update required";
        if (hasMinDriverMajor) {
            m_unavailableReason += ": minimum " + std::to_string(minDriverMajor);
            if (hasMinDriverMinor) {
                m_unavailableReason += "." + std::to_string(minDriverMinor);
            }
        }
        m_unavailableReason += ")";
    } else {
        m_unavailableReason += " (driver or hardware not supported)";
    }

    if (hasFeatureInitResult) {
        m_unavailableReason += "; NGX feature init result=" + std::to_string(featureInitResult);
    }

    return false;
}

bool NgxContext::queryDlssFGAvailability() {
    if (m_parameters == nullptr) {
        m_dlssFGAvailable = false;
        m_maxMultiFrameCount = 0;
        return false;
    }

    int available = 0;
    if (readIntParameter(m_parameters, NVSDK_NGX_Parameter_FrameGeneration_Available, available) ||
        readIntParameter(m_parameters, NVSDK_NGX_Parameter_FrameInterpolation_Available, available)) {
        m_dlssFGAvailable = available != 0;
    } else {
        m_dlssFGAvailable = false;
        m_maxMultiFrameCount = 0;
        return false;
    }

    if (!m_dlssFGAvailable) {
        m_maxMultiFrameCount = 0;
        return false;
    }

    int maxMultiFrameCount = 0;
    if (readIntParameter(m_parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, maxMultiFrameCount) &&
        maxMultiFrameCount > 0) {
        m_maxMultiFrameCount = maxMultiFrameCount;
    } else {
        m_maxMultiFrameCount = 1;
    }

    return true;
}
