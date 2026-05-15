#pragma once

#include "cli/config.h"

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <cstdint>
#include <string>

struct VkInstance_T;
using VkInstance = VkInstance_T*;
struct VkPhysicalDevice_T;
using VkPhysicalDevice = VkPhysicalDevice_T*;
struct VkDevice_T;
using VkDevice = VkDevice_T*;
struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

struct DlssPresetConfig {
    DlssSRPreset globalPreset = DlssSRPreset::Auto;
    DlssSRPreset dlaa = DlssSRPreset::Auto;
    DlssSRPreset quality = DlssSRPreset::Auto;
    DlssSRPreset balanced = DlssSRPreset::Auto;
    DlssSRPreset performance = DlssSRPreset::Auto;
    DlssSRPreset ultraPerformance = DlssSRPreset::Auto;
    DlssSRPreset ultraQuality = DlssSRPreset::Auto;
};

struct DlssPresetSlots {
    int dlaa = 0;
    int quality = 0;
    int balanced = 0;
    int performance = 0;
    int ultraPerformance = 0;
    int ultraQuality = 0;
};

DlssPresetConfig makeDlssPresetConfig(const AppConfig& config);
int resolveDlssPresetValue(DlssSRPreset perQualityPreset,
                           DlssSRPreset globalPreset,
                           NVSDK_NGX_PerfQuality_Value quality);
DlssPresetSlots resolveDlssPresetSlots(const DlssPresetConfig& presets);

class NgxContext {
public:
    NgxContext();
    ~NgxContext();

    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkCommandBuffer cmdBuf,
              std::string& errorMsg,
              NVSDK_NGX_Logging_Level logLevel = NVSDK_NGX_LOGGING_LEVEL_OFF);

    bool isDlssSRAvailable() const;
    std::string unavailableReason() const;

    bool createDlssSR(int inputWidth,
                      int inputHeight,
                       int outputWidth,
                       int outputHeight,
                       DlssQualityMode quality,
                       const AppConfig& config,
                       VkCommandBuffer cmdBuf,
                       std::string& errorMsg);

    void releaseDlssSR();
    bool isDlssFGAvailable() const;
    bool createDlssFG(unsigned int width,
                      unsigned int height,
                      unsigned int backbufferFormat,
                      VkCommandBuffer cmdBuf,
                      std::string& errorMsg);
    void releaseDlssFG();
    void shutdown();

    NVSDK_NGX_Handle* featureHandle() const;
    NVSDK_NGX_Handle* fgFeatureHandle() const;
    NVSDK_NGX_Handle* getFeatureHandle() const;
    NVSDK_NGX_Parameter* parameters() const;
    int maxMultiFrameCount() const;

    bool isInitialized() const;

    /// API version that successfully initialized NGX (0 if not yet initialized).
    /// Either kNgxApiVersionPrimary or kNgxApiVersionFallback (defined in .cpp).
    uint32_t ngxApiVersionUsed() const;

    /// True if init() succeeded only after falling back to API_VERSION - 1.
    bool ngxUsedFallback() const;

    /// Returns version string of nvngx_dlss.dll currently loaded in this
    /// process (e.g. "310.5.3.0"). Returns "not loaded" / "version unavailable"
    /// when the DLL is not yet mapped or its version resource is absent. Safe
    /// to call without GPU; uses GetModuleHandleW (does not LoadLibrary).
    std::string dlssDllVersion() const;

private:
    NVSDK_NGX_PerfQuality_Value mapQuality(DlssQualityMode quality) const;
    bool queryDlssSRAvailability();
    bool queryDlssFGAvailability();

    NVSDK_NGX_Handle* m_featureHandle = nullptr;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
    VkDevice m_device = nullptr;
    bool m_initialized = false;
    bool m_dlssSRAvailable = false;
    NVSDK_NGX_Handle* m_fgFeatureHandle = nullptr;
    bool m_dlssFGAvailable = false;
    int m_maxMultiFrameCount = 0;
    std::string m_unavailableReason;
    uint32_t m_ngxApiVersionUsed = 0;
    bool m_ngxUsedFallback = false;
};
