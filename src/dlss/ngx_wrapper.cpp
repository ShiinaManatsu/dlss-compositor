#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include "dlss/ngx_wrapper.h"

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

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kNgxWorkDir[] = L".";
constexpr char kNgxProjectId[] = "6c6f53ec-6f25-4f9f-8d71-2f0f3c5e7a11";
constexpr char kNgxEngineVersion[] = "0.1.0";

void NVSDK_CONV ngxLogCallback(const char* message,
                               NVSDK_NGX_Logging_Level,
                               NVSDK_NGX_Feature) {
    std::fprintf(stderr, "[NGX] %s\n", message != nullptr ? message : "(null)");
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

} // namespace

NgxContext::NgxContext() = default;

NgxContext::~NgxContext() {
    shutdown();
}

bool NgxContext::init(VkInstance instance,
                      VkPhysicalDevice physicalDevice,
                      VkDevice device,
                      VkCommandBuffer cmdBuf,
                      std::string& errorMsg) {
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
    featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;

    const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(kNgxProjectId,
                                                                              NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                              kNgxEngineVersion,
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
    queryDlssSRAvailability();
    queryDlssFGAvailability();
    return true;
}

bool NgxContext::isDlssSRAvailable() const {
    return m_initialized && m_dlssSRAvailable;
}

bool NgxContext::isDlssFGAvailable() const {
    return m_initialized && m_dlssFGAvailable;
}

std::string NgxContext::unavailableReason() const {
    return m_unavailableReason;
}

bool NgxContext::createDlssSR(int inputWidth,
                              int inputHeight,
                              int outputWidth,
                              int outputHeight,
                              DlssQualityMode quality,
                              DlssSRPreset preset,
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
    createParams.Feature.InPerfQualityValue = mapQuality(quality);
    createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    createParams.InEnableOutputSubrects = false;

    const int presetValue = static_cast<int>(preset);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, presetValue);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, presetValue);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, presetValue);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, presetValue);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, presetValue);
    NVSDK_NGX_Parameter_SetI(m_parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, presetValue);

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
